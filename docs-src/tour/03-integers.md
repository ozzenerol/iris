# Integers

The integer types, how integer literals are written, how arithmetic works on them, and how values move from one type to another.

## The types

| Type | Bytes | Smallest | Largest |
|---|---|---|---|
| `i8` | 1 | -128 | 127 |
| `i16` | 2 | -32768 | 32767 |
| `i32` | 4 | -2147483648 | 2147483647 |
| `i64` | 8 | -9223372036854775808 | 9223372036854775807 |
| `u8` | 1 | 0 | 255 |
| `u16` | 2 | 0 | 65535 |
| `u32` | 4 | 0 | 4294967295 |
| `u64` | 8 | 0 | 18446744073709551615 |

`i64` is the integer to use unless there is a reason for another: it is the type a number gets by default, and the type of lengths and indexes. `u8` is a byte; text is made of them. The others suit data with a fixed layout, like a file format or a network protocol, and C functions that take them.

Each type knows its limits as `min` and `max`:

```iris run
void main() {
    print("{i8.min} {i8.max}")
    print("{u16.max} {i32.max}")
    print("{i64.max} {u64.max}")
}
```

```output
-128 127
65535 2147483647
9223372036854775807 18446744073709551615
```

## Literals

A number can be written in decimal, hexadecimal (`0x`), binary (`0b`) or octal (`0o`). `_` can go between digits to make long numbers easier to read; it has no meaning of its own:

```iris run
void main() {
    print("{1_000_000} {0xff} {0xFF_FF} {0b1010} {0o755}")
}
```

```output
1000000 255 65535 10 493
```

A character in single quotes is the number of its byte, a `u8`:

```iris run
void main() {
    u8 a = 'a'
    digit := '7' - '0'
    print("{a} {digit} {'\n'}")
}
```

```output
97 7 10
```

The escapes inside quotes are `\n` (newline), `\t` (tab), `\r` (carriage return), `\0` (zero), `\\`, `\'`, `\"` and `\x41` (the byte written in two hex digits).

A literal has no type of its own until it is used. With nothing to go by, it is an `i64`. Given to a typed variable, a parameter or an operator with a typed value on the other side, it takes that type, as long as it fits:

```iris run
void takeByte(u8 b) {
    print("got {b}")
}

void main() {
    u16 port = 8080
    takeByte(255)
    i32 small = -40
    print("{port + 1} {small * 2}")
}
```

```output
got 255
8081 -80
```

A literal that doesn't fit is a compile error, so `u8 b = 256` doesn't compile.

## Units

A number can carry a unit. A duration is a number of nanoseconds, with the type `Duration`: `ns`, `us`, `ms`, `s`, `m` and `h`. A size is a plain integer: `KiB`, `MiB` and `GiB` multiply by 1024, 1024 squared and 1024 cubed:

```iris run
void main() {
    timeout := 90s
    poll := 250ms
    print("{timeout} {poll} {i64(poll)}")
    buf := 64KiB
    print("{buf} bytes")
}
```

```output
1m30s 250ms 250000000
65536 bytes
```

A literal has one unit; `90s` or `1m + 30s` both make a minute and a half. A `Duration` prints in its units, and `i64(d)` gives its nanoseconds. Durations add and subtract, and multiply and divide by plain numbers. Dividing one duration by another gives a plain number, how many times the second fits in the first:

```iris run
void main() {
    timeout := 1m + 30s
    print("{timeout} {timeout * 2} {timeout / 4}")
    print("{timeout / 1s} seconds, {timeout / 250ms} polls")
}
```

```output
1m30s 3m0s 22.5s
90 seconds, 360 polls
```

The [time](../std/index.html#time) module has the time of day, dates and a clock.

## Arithmetic

`+`, `-`, `*`, `/` and `%` work on two values of the same integer type and give that type. Division drops the fraction, toward zero, and `%` gives what is left, with the sign of the left side:

```iris run
void main() {
    print("{7 / 2} {-7 / 2} {7 % 3} {-7 % 3}")
    a := 17
    b := 5
    print("{a / b} remainder {a % b}")
}
```

```output
3 -3 1 -1
3 remainder 2
```

The bit operators are `&` (and), `|` (or), `^` (exclusive or), `~` (each bit flipped), and the shifts `<<` and `>>`. Shifting a signed value right keeps its sign:

```iris run
void main() {
    n := 0b1100
    print("{n & 0b1010} {n | 1} {n ^ 0b0110} {~n}")
    print("{1 << 10} {n >> 2} {-16 >> 2}")
    u8 b = 0xf0
    print("{~b} {b >> 4}")
}
```

```output
8 13 10 -13
1024 3 -4
15 15
```

## Overflow and division by zero

A result that doesn't fit in its type is an overflow. In a debug build, which is what `iris run` makes, it stops the program with a message that says where:

```iris
u8 a = 250
u8 b = 10
c := a + b // panic: integer overflow
```

A `release-fast` build doesn't check, and the value wraps around: `250 + 10` in a `u8` gives `4`. `release-safe` keeps the check. Dividing by zero stops the program in every build.

Arithmetic happens in the type of the values. Two `u8` values add up as a `u8`, even when the result goes into an `i64`, so the sum of two big bytes overflows. Convert first when the result needs more room:

```iris run
void main() {
    u8 a = 250
    u8 b = 10
    i64 sum = i64(a) + i64(b)
    print("{sum}")
}
```

```output
260
```

## Converting

A value converts to another integer type on its own when that type holds every value it could have: `u8` to `u16`, `i32` to `i64`, `u32` to `i64`. Every other conversion is written out, with the type's name used like a function:

```iris run
void main() {
    u8 small = 200
    i64 wide = small // on its own: every u8 fits in an i64
    i64 n = 65
    u8 letter = u8(n) // written out: an i64 may not fit
    u64 big = u64(wide) * 3
    print("{wide} {letter} {big}")
}
```

```output
200 65 600
```

A conversion that can lose the value is checked: in a debug build, `u8(300)` with a value known only when the program runs stops it with a message. A signed value never converts on its own to an unsigned type, since a negative number has no unsigned value.

The two sides of an operator must have the same type, or one must convert on its own to the other. `i32 + u32` doesn't compile: neither holds all the values of the other, so the program says which one it means.

```iris error
i32 a = 5
u32 b = 6
c := a + b // error: '+' can't mix i32 and u32; convert one side
```

## Comparing

`==`, `!=`, `<`, `<=`, `>` and `>=` compare two integers and give a `bool`. They follow the same rule as arithmetic: both sides of one type, or one that converts to the other.

```iris run
void main() {
    u8 b = 200
    i64 limit = 100
    print("{b > limit} {b == 200} {limit != 100}")
}
```

```output
true true false
```
