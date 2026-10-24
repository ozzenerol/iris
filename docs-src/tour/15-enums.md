# Enums

An enum is a type with a fixed list of values. Plain enums name choices and codes. Flags enums hold any set of their values. Enums with data carry fields per value.

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

A value is written `Level.warn`. Where the type is known (a typed variable, a parameter, a comparison with the enum, a `match` arm), write just `.warn`:

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

An enum interpolates as its value's name. `.name` returns the name as a `str`.

## Numbers

Values are numbered from 0 up. A value can set its own number, and later values continue from it. `.value` returns the number as an `i64`.

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

Numbers fit in an `i32`. A number never converts to an enum implicitly. Map it with a `match`:

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

## match

A `match` on an enum needs an arm per value or a `_` arm, so a new value shows every place that must handle it:

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

A `flags` enum numbers its values as powers of two, so one value can hold several. `|` combines, `&` intersects, `~` inverts, and `has` tests one.

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

A flags value prints its set names joined by `|`. A flags enum has at most 31 values.

## Enums with data

A value can carry data, declared like parameters:

```iris
enum Frame {
    Data(i64 stream, i64 size)
    Ping(u64 payload)
    Close
}
```

Create a value by calling it. Unpack it with `match`: each arm names a variant and binds its data.

```iris run
// Protocol frames, each with a 9-byte header.
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

`_` skips a field. A variant name alone matches whatever its data. A value prints as its variant's name.

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

The data is part of the value, which copies like a struct. Enums with data don't support `==`. Use `match`.

## Error codes

An error's code is a plain enum value. A module lists its failures as an enum and callers match on it (see [Errors](18-errors.html)):

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
