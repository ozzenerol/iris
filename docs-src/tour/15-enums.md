# Enums

An enum is a type with a fixed list of values. Plain enums name choices and codes; flag enums hold any set of their values at once; and the values of an enum with data each carry fields of their own.

## Plain enums

An enum lists its values in braces:

```iris run
enum Level { debug, info, warn, error }

void main() {
    Level l = Level.warn
    print("level {l}")
    if l == Level.warn {
        print("watch out")
    }
}
```

```output
level warn
watch out
```

A value is written with its enum's name in front: `Level.warn`. Where the type is known from the place it goes (a typed variable, a parameter, a comparison with a value of the enum, a `match` arm), the name can be left out, keeping just the dot:

```iris run
enum Level { debug, info, warn, error }

void log(Level level, str msg) => print("[{level}] {msg}")

void main() {
    Level l = .info
    log(.error, "disk full")
    log(l, "started")
    print("{l == .info}")
}
```

```output
[error] disk full
[info] started
true
```

An enum printed in text writes its value's name. `.name` gives the name as a `str`.

## Numbers

Each value is a number, starting at 0 and going up by one. A value can be given its own number, and the ones after it go on from there. `.value` gives the number as an `i64`:

```iris run
enum Status { ok = 200, created, notFound = 404, error = 500 }

void main() {
    s := Status.created
    print("{s} is {s.value}")
    print("{Status.notFound.value} {Status.error.name}")
}
```

```output
created is 201
404 error
```

The numbers fit in an `i32`. Numbers don't turn into enum values on their own: the program says which value a number means, with a `match`:

```iris run
enum Status { ok = 200, notFound = 404, error = 500 }

Status statusOf(i64 code) => match code {
    200 => .ok
    404 => .notFound
    _ => .error
}

void main() {
    print("{statusOf(404)} {statusOf(301)}")
}
```

```output
notFound error
```

## match on an enum

A `match` on an enum must have an arm for each value, or a `_` arm, so adding a value later shows every place that has to handle it:

```iris run
enum Level { debug, info, warn, error }

str color(Level l) => match l {
    .debug, .info => "gray"
    .warn => "yellow"
    .error => "red"
}

void main() {
    print("{color(.info)} {color(.error)}")
}
```

```output
gray red
```

## Flags

An enum marked `flags` gives its values powers of two, so a value of it can hold any of them together. `|` joins them, `&` keeps what two have in common, `~` turns every one around, and `has` says whether one is there:

```iris run
enum Perm flags { read, write, exec }

void main() {
    Perm p = .read | .write
    print("{p} has write: {p.has(.write)}, exec: {p.has(.exec)}")
    p = p | .exec
    noWrite := p & ~Perm.write
    print("{p} -> {noWrite}")
    Perm none
    print("[{none}] {none.value} {p.value}")
}
```

```output
read|write has write: true, exec: false
read|write|exec -> read|exec
[0] 0 7
```

A flags value prints the names of the values that are set, joined by `|`. A flags enum can have up to 31 values.

## Enums with data

A value of an enum can carry data, written like a function's parameters after its name:

```iris
enum Frame {
    Data(i64 stream, i64 size)
    Ping(u64 payload)
    Close
}
```

A value is made by calling it like a function, and taken apart with `match`. Each arm names a value and gives names to its data, which the arm then uses:

```iris run
// The frames of a protocol, each with a 9-byte header.
enum Frame {
    Data(i64 stream, i64 size)
    Ping(u64 payload)
    Close
}

i64 wireSize(Frame f) => match f {
    Data(stream, size) => 9 + size
    Ping(payload) => 9 + 8
    Close => 9
}

void main() {
    Frame[3] frames = [Frame.Data(1, 1200), .Ping(42), .Close]
    for f in frames {
        print("{f}: {wireSize(f)} bytes")
    }
}
```

```output
Data: 1209 bytes
Ping: 17 bytes
Close: 9 bytes
```

A name in an arm can be `_` for data that isn't needed, and a value's name on its own takes it whatever its data. A value prints as its name:

```iris run
// What a poller reports about a socket.
enum Event {
    Readable(i32 fd, i64 bytes)
    Writable(i32 fd)
    Hangup(i32 fd)
}

void handle(Event e) {
    match e {
        Readable(fd, _) => print("fd {fd} has data")
        Writable(fd) => print("fd {fd} can be written")
        Hangup => print("a peer went away")
    }
}

void main() {
    handle(.Readable(5, 512))
    handle(.Writable(6))
    handle(Event.Hangup(5))
}
```

```output
fd 5 has data
fd 6 can be written
a peer went away
```

The data is part of the value, which is copied as a whole like a struct. Values of an enum with data can't be compared with `==`, since their data may differ; `match` is the way to look at them.

## Errors are enums

The code of an error is a value of a plain enum. A module lists what can go wrong with it as an enum, and the caller matches on it (see [Errors](18-errors.html)):

```iris run
enum ConfigError { missing, invalid }

!i64 port(str text) {
    if text.len == 0 {
        return error(ConfigError.missing, "no port given")
    }
    return 8080
}

void main() {
    _ = port("") catch e {
        match e.code {
            ConfigError.missing => print("using the default")
            _ => print("other: {e.message}")
        }
        0
    }
}
```

```output
using the default
```
