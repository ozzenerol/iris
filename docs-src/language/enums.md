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

Every value must have an arm, or there must be a `_`.

## Flags

An enum marked `flags` gives its values powers of two, so they can be combined with `|`. `has` checks for a value:

```iris
enum Perm flags { read, write, exec }

Perm p = .read | .write
if p.has(.write) { }
```
