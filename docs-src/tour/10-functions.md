# Functions

Declaring functions, default and named arguments, variadic parameters, and returning several values.

## Declaring

A function is its return type, name, parameters and block. `void` returns nothing.

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

Adjacent parameters of one type can share it: `i64 frameSize(i64 payload, headers)`.

A function with a result must return on every path. Reaching the closing brace without `return` is a compile error.

## One expression

A body that is one expression can use `=>`, without braces or `return`:

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

In a `void` function, the expression after `=>` is a call.

## Order and recursion

Functions can appear in any order and in any file of the module. A function can call itself.

```iris run
import str

// Prints a DNS name's labels from the top down, as a resolver walks them.
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

Each name refers to one function, so there is no overloading. A function can't take the name of a built-in (`print`, `format`, `free`...) or a type.

## Arguments are copies

Each argument is copied into its parameter. Changing the parameter leaves the caller's variable alone.

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

To change the caller's value, pass a pointer with `&` and use `*` inside (see [Pointers](17-pointers.html)):

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

Structs are copied too. Pass a large struct, or one the function changes, as a pointer. Fields are reached with a dot through a pointer.

## Defaults

A parameter can have a default, used when the call omits it. Parameters with defaults come last.

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

A default must be known at compile time: a number, text, a constant, or `[]`.

## Named arguments

Any argument can be passed by name. This skips defaults and clarifies calls with several values of one type.

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

Named arguments follow positional ones, in any order. Each parameter is given once.

## Variadic parameters

A last parameter `T... name` takes all remaining arguments as a slice `T[]`. Pass an existing slice by the parameter's name.

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

- The arguments go into a stack array, so nothing is allocated.
- A `...` parameter is last and has no default. With no extra arguments it is empty.
- As a function value, `total` has type `i64(i64[])`.

## Several results

The return type lists the types in parentheses. `return` gives the values in parentheses.

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

The caller unpacks every value at the call, one name each, with `_` for unwanted ones.

## Functions that can fail

A return type starting with `!` means the function returns a value or an error. The caller must handle it with `try` or `catch`.

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

[Errors](18-errors.html) covers them in full. Results of other functions can be ignored:

```iris run
// Sends a line and returns its byte count.
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
