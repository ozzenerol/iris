# Values in text

Interpolating values with braces, the `format` function, and how temporary text is freed.

## Braces

An expression in braces inside a string literal is replaced by its value as text:

```iris run
void main() {
    host := "10.0.0.5"
    sent := 3
    rtt := 4.5

    print("{sent} pings to {host}, {f64(sent) * rtt}ms in all")
    print("{sent > 2} {sent * 64} bytes")
}
```

```output
3 pings to 10.0.0.5, 13.5ms in all
true 192 bytes
```

The compiler type-checks each expression in braces. Write a literal brace as `{{` or `}}`. A raw string (`r"..."`) keeps braces as they are.

## What can be interpolated

| Type | Written as |
|---|---|
| integers | decimal: `42`, `-7` |
| `f32`, `f64` | shortest digits that read back the same: `0.1`, `2.5e-8` |
| `bool` | `true` or `false` |
| `str`, `String` | the text |
| `Duration` | in units: `1.5s`, `2m0s`, `250ms` |
| plain enum | the value's name: `warn` |
| flags enum | the set names: `read\|write` |
| enum with data | the variant's name: `Closed` |
| error code | enum and value: `NetError.timeout` |
| struct with `String format()` | the method's result |

```iris run
enum State { connecting, open, closed }

enum Perm flags { read, write, exec }

void main() {
    State s = .open
    Perm p = .read | .write

    print("{s} {p} {1500ms} {0.25} {false}")
}
```

```output
open read|write 1.5s 0.25 false
```

Pointers, arrays and slices other than bytes can't be interpolated. Use a loop or a method.

## Format specs

A `:` after the expression starts a spec: `{port:>6}`. Its parts come in this order, each optional:

| Part | Effect |
|---|---|
| fill character and `<`, `>` or `^` | align left, right or centre in the width; fill defaults to a space |
| `+` | `+` before positive numbers |
| `0` | pad numbers with zeros after the sign |
| width | minimum characters |
| `.` and a number | digits after the point for floats; maximum characters for text |
| `x`, `X`, `o`, `b` | hex, upper-case hex, octal, binary |
| `e`, `f` | exponent or fixed-point float, 6 digits unless a precision is given |

Numbers align right by default, everything else left. Width and precision count characters, so `Köln` is four wide.

```iris run
void main() {
    print("[{8080:>6}] [{"GET":<6}] [{"ok":^6}] [{"Köln":*>8}]")
    print("{7:03} {-42:06} {255:x} {255:#>6X} {5:08b} {12:+}")

    loss := 0.256789
    print("{loss:.2} {loss:.3e} {2.5:f} {1e-9:e}")
    print("[{"ingress-controller":.7}] [{true:>6}] [{250ms:>8}]")
}
```

```output
[  8080] [GET   ] [  ok  ] [****Köln]
007 -00042 ff ####FF 00000101 +12
0.26 2.568e-01 2.500000 1.000000e-09
[ingress] [  true] [   250ms]
```

- A width works on any value, including structs with `format`.
- Other parts must fit the type: `{1.5:x}` and `{"a":+}` don't compile.
- A `:` inside parentheses, brackets or quotes belongs to the expression, so `{pad(text, width: 4)}` and `{line[0:2]}` work.
- Text in a constant can't have a spec. Use `format` at run time.

## A struct's own text

A struct with a method `String format()` can be interpolated. The returned `String` is freed after the statement.

```iris run
struct Ip4 {
    u8[4] b
}

String Ip4.format() => format("{self.b[0]}.{self.b[1]}.{self.b[2]}.{self.b[3]}")

struct Route {
    Ip4 dest
    u8 prefix
    Ip4 via
}

String Route.format() => format("{self.dest}/{self.prefix} via {self.via}")

void main() {
    gw := Ip4{b: [192, 168, 1, 1]}
    r := Route{dest: Ip4{b: [10, 0, 0, 0]}, prefix: 8, via: gw}

    print("gateway {gw}")
    print("route {r}")
}
```

```output
gateway 192.168.1.1
route 10.0.0.0/8 via 192.168.1.1
```

## format

`format` builds the same text as `print` and returns it as a new `String` for the caller to free:

```iris run
void main() {
    host := "localhost"
    port := 8080
    scoped String address = format("{host}:{port}")

    print("listening on {address}")
    print("{address.len} bytes")
}
```

```output
listening on localhost:8080
14 bytes
```

`str.format` is the same function. `panic` takes the same text. `io.eprint` writes it to standard error with a newline, `io.write` to standard output without one.

## Text passed to a function

Interpolated text can go straight to any `str` parameter. It is built in a temporary `String` freed at the end of the statement:

```iris run
void logLine(str s) {
    print("[edge-1] {s}")
}

void main() {
    open := 3

    logLine("{open} connections open")
    logLine("plain text")
}
```

```output
[edge-1] 3 connections open
[edge-1] plain text
```

A returned `String` passed straight to a `str` parameter is freed after the statement too:

```iris run
import str

void main() {
    print(str.upper("http/1.1 200 ok"))
    print(str.repeat("-", 15))
}
```

```output
HTTP/1.1 200 OK
---------------
```

Everywhere else, interpolated text needs `format` and a returned `String` needs a variable.

A call that returns a `str`, slice or pointer may return part of its argument. If that argument is a temporary, the result would point into freed text, so the compiler rejects it:

```iris error
t := str.trim("  {name}  ") // error: this text is freed when the statement ends, and the call gives back a str that may point into it
```

```iris
scoped String padded = format("  {name}  ")
t := str.trim(padded)
```

## Building text in parts

`String.append` adds to the end of a `String`, so long text is built without repeated copies:

```iris run
import io

void main() {
    scoped String out = "GET / HTTP/1.1\n".copy()
    headers := ["Host", "Accept", "Connection"]
    values := ["example.com", "*/*", "close"]

    for i, h in headers {
        scoped String line = format("{h}: {values[i]}\n")
        out.append(line)
    }

    io.write(out)
}
```

```output
GET / HTTP/1.1
Host: example.com
Accept: */*
Connection: close
```

`str.join` joins parts with a separator (see [Text](06-text.html)).
