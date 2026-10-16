# Variables and types

How to declare values, and the types Iris has built in.

## Declaring a variable

A declaration gives the type, then the name, then the value:

```iris
i64 count = 10
str name = "iris"
```

`:=` declares a variable and takes its type from the value:

```iris
count := 10        // i64
ratio := 0.5       // f64
ok := true         // bool
name := "iris"     // str
```

A variable declared without a value starts at zero: `0`, `false`, an empty `str` or a `null` pointer.

```iris
i64 total
bool done
```

## Constants

`const` gives a name to a value that never changes. The value is worked out when the program is compiled.

```iris
const MAX_CONN = 1024
const VERSION = "0.1.0"
```

## Numbers

| Type | Size | Range |
|---|---|---|
| `i8`, `i16`, `i32`, `i64` | 1, 2, 4, 8 bytes | signed integers |
| `u8`, `u16`, `u32`, `u64` | 1, 2, 4, 8 bytes | integers from 0 up |
| `f32`, `f64` | 4, 8 bytes | floating point |

An integer literal is an `i64` and a literal with a point is an `f64`. Long numbers can use `_` between digits:

```iris
limit := 1_000_000
mask := 0xff
flags := 0b1010
```

## bool

`bool` is `true` or `false`. Conditions must be `bool`: `if n` is an error, `if n != 0` is correct.

## Converting

A number converts on its own to a type that holds every value it can have, such as `u8` to `i64`. Every other conversion is written as the type's name used like a function:

```iris
u8 b = 200
i64 n = b              // automatic: no value can be lost
u8 small = u8(n)       // written out: n might not fit
f64 avg = f64(total) / f64(count)
```

In a debug build, a conversion that loses a value stops the program with an error.

## Durations and sizes

Numbers can carry a unit. Durations are `ns`, `us`, `ms`, `s`, `m` and `h`, and their type is `Duration`. Sizes are `KiB`, `MiB` and `GiB`, and they are plain integers.

```iris
sleep(200ms)
timeout := 30s
buf := alloc(u8, 64KiB)
```

## Type aliases

`type` gives another name to a type. The two names mean the same type.

```iris
type Port = u16
type Handler = void(Request *, Response *)
```

## Scope

A variable exists from its declaration to the end of its block. A variable in an inner block can't have the same name as one in an outer block of the same function.
