# Values in text

Putting values into text with braces, the `format` function that keeps the result, and how the temporary text is freed for you.

## Braces

A name or an expression in braces inside a string literal is replaced by its value, written as text:

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

The compiler checks every value in braces, with its type, as it checks any other expression. A brace on its own is written twice, `{{` and `}}`, and a raw string (`r"..."`) takes braces as they are.

## What can be put into text

| Type | Written as |
|---|---|
| integers | in decimal: `42`, `-7` |
| `f32`, `f64` | the fewest digits that read back as the same number: `0.1`, `2.5e-8` |
| `bool` | `true` or `false` |
| `str`, `String` | the text itself |
| `Duration` | in units: `1.5s`, `2m0s`, `250ms` |
| a plain enum | the value's name: `warn` |
| a flags enum | the names that are set: `read\|write` |
| an enum with data | the value's name: `Closed` |
| an error's code | the enum and the value: `NetError.timeout` |
| a struct with `String format()` | what the method gives |

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

A pointer, an array or a slice of anything but bytes can't be put into text as it is; the program says what it means with a loop or a method.

## A struct's own text

A struct with a method `String format()` can be put into text. The method gives the text, and the `String` it returns is freed after the statement:

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

`print` writes text with values to standard output. `format` builds the same text and gives it as a new `String`, which the caller frees:

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

`str.format` is the same as `format`, by the `str` module's name.

`panic` takes the same text, and `io.eprint` writes it to standard error with a newline. `io.write` writes to standard output without one.

## Text passed straight to a function

Text with values can be given to any function that takes a `str`. It is built in a temporary `String`, which is freed when the statement ends:

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

A function that returns a new `String`, given straight to a function that takes a `str`, is freed after the statement too:

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

Anywhere else, text with values needs `format`, and a `String` from a call needs a variable. The temporary would otherwise be gone before it is used.

A call that gives back a `str`, a slice or a pointer may give back part of what it was given. When that is a temporary, the result would point into text that is freed at the end of the statement, so the compiler doesn't allow it:

```iris error
t := str.trim("  {name}  ") // error: this text is freed when the statement ends, and the call gives back a str that may point into it
```

```iris
scoped String padded = format("  {name}  ")
t := str.trim(padded)
```

## Building text a part at a time

`String.append` adds to the end of a `String`. With `format` for the parts, longer text is built without copying it again and again:

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


`str.join` puts a list of parts together with a separator between them (see [Text](06-text.html)).
