# Enums

A type with a fixed list of values. Each value can carry data of its own.

## Plain enums

```iris
enum Level { debug, info, warn, error }

Level l = Level.warn
```

When the type is already known, the value can be written with a dot and no type name:

```iris
Level l = .warn
log(.error, "disk full")
```

Each value is a number, starting at 0. A value can be given its number:

```iris
enum Status { ok = 200, notFound = 404, error = 500 }
```

`.name` gives the value's name as a `str`, and enums can be printed:

```iris
print("level is {l}")       // level is warn
```

A `match` on an enum needs an arm for every value, or a `_` arm:

```iris run
enum Level { debug, info, warn, error }

str weight(Level l) => match l {
    .debug, .info => "low"
    .warn => "medium"
    .error => "high"
}

void main() {
    Level l = .warn
    print("{l} is {weight(l)}")
    print("{l.name.len} letters")
}
```

```output
warn is medium
4 letters
```

## Enums that carry data

A value can carry data, written like a function's parameters:

```iris
enum Packet {
    Ping
    Data(u8[] bytes)
    Close(u16 code, str reason)
}

Packet p = Packet.Data(payload)
Packet q = .Close(1000, "bye")
```

The data is read with `match`. Each arm names the value and gives names to its data:

```iris
match p {
    Ping => send(.Pong)
    Data(bytes) => handle(bytes)
    Close(code, reason) => print("closed {code}: {reason}")
}
```

Every value must have an arm, or there must be a `_`. A field you don't need is named `_`, and a value's name without parentheses matches it whatever its data:

```iris
match p {
    Data(_) => print("some data")
    Close => print("closed")
    _ => {}
}
```

Values of an enum with data can't be compared with `==`. Use `match`.

A complete program:

```iris run
enum Shape {
    Circle(i64 r)
    Rect(i64 w, i64 h)
    Empty
}

i64 area(Shape s) => match s {
    Circle(r) => 3 * r * r
    Rect(w, h) => w * h
    Empty => 0
}

void main() {
    Shape[3] shapes = [Shape.Circle(2), .Rect(5, 5), .Empty]
    for s in shapes {
        print("{s}: {area(s)}")
    }
}
```

```output
Circle: 12
Rect: 25
Empty: 0
```

## Flags

An enum marked `flags` gives its values powers of two, so they can be combined with `|`. `has` checks for a value:

```iris
enum Perm flags { read, write, exec }

Perm p = .read | .write
if p.has(.write) { }
```

`&`, `|`, `^` and `~` work on flags, and printing shows the names of the values that are set:

```iris run
enum Perm flags { read, write, exec }

void main() {
    Perm p = .read | .write
    print("{p}, write: {p.has(.write)}, exec: {p.has(.exec)}")
}
```

```output
read|write, write: true, exec: false
```

A flag enum can have up to 31 values.
