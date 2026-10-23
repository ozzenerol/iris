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
    mtu := 1_500
    ethertype := 0x0800   // IPv4
    mask := 0xFF_FF_FF_00 // 255.255.255.0
    flags := 0b0001_0010  // SYN and ACK
    mode := 0o644         // rw-r--r--
    print("{mtu} {ethertype} {mask} {flags} {mode}")
}
```

```output
1500 2048 4294967040 18 420
```

A character in single quotes is the number of its byte, a `u8`:

```iris run
void main() {
    u8 cr = '\r'
    status := "503"
    first := status[0] - '0' // 5: a server error
    print("{cr} {'\n'} {first}")
}
```

```output
13 10 5
```

The escapes inside quotes are `\n` (newline), `\t` (tab), `\r` (carriage return), `\0` (zero), `\\`, `\'`, `\"` and `\x41` (the byte written in two hex digits).

A literal has no type of its own until it is used. With nothing to go by, it is an `i64`. Given to a typed variable, a parameter or an operator with a typed value on the other side, it takes that type, as long as it fits:

```iris run
void setTtl(u8 ttl) {
    print("ttl {ttl}")
}

void main() {
    u16 port = 8080
    setTtl(64)
    i32 offset = -40
    print("{port + 1} {offset * 2}")
}
```

```output
ttl 64
8081 -80
```

A literal that doesn't fit is a compile error, so `setTtl(256)` doesn't compile.

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
    size := 4000
    mss := 1460
    print("{size / mss} full segments and {size % mss} bytes left")
}
```

```output
3 -3 1 -1
2 full segments and 1080 bytes left
```

The bit operators are `&` (and), `|` (or), `^` (exclusive or), `~` (each bit flipped), and the shifts `<<` and `>>`. Shifting a signed value right keeps its sign:

```iris run
void main() {
    u32 ip = 0xC0A8_0A2A   // 192.168.10.42
    u32 mask = 0xFFFF_FF00 // a /24 network
    net := ip & mask
    print("network {net >> 24}.{(net >> 16) & 0xff}.{(net >> 8) & 0xff}.{net & 0xff}")
    print("host {ip & ~mask}")
    u8 flags = 0x02      // SYN
    flags = flags | 0x10 // and ACK
    print("{flags} {flags ^ 0x02} {flags >> 4} {1 << 10} {-16 >> 2}")
}
```

```output
network 192.168.10.0
host 42
18 16 1 1024 -4
```

## Overflow and division by zero

A result that doesn't fit in its type is an overflow. In a debug build, which is what `iris run` makes, it stops the program with a message that says where:

```iris
u8 ttl = 250
next := ttl + 10 // panic: integer overflow
```

The release builds don't check overflow. An unsigned value wraps around: `250 + 10` in a `u8` gives `4`. A signed value wraps around in a `release-safe` build. A `release-fast` build assumes signed math never overflows, as the debug build has checked, and uses that to make loops faster; there a signed overflow gives a result that can't be relied on. Dividing by zero stops a debug and a `release-safe` build; in `release-fast` it is not checked, and the result can't be relied on.

Arithmetic happens in the type of the values. Two `u8` values add up as a `u8`, even when the result goes into an `i64`, so the sum of two big bytes overflows. Convert first when the result needs more room:

```iris run
void main() {
    u8 ipHeader = 60 // the IHL field, in bytes
    u8 tcpHeader = 60
    u8 options = 200
    i64 total = i64(ipHeader) + i64(tcpHeader) + i64(options)
    print("{total} bytes of headers")
}
```

```output
320 bytes of headers
```

## Converting

A value converts to another integer type on its own when that type holds every value it could have: `u8` to `u16`, `i32` to `i64`, `u32` to `i64`. Every other conversion is written out, with the type's name used like a function:

```iris run
void main() {
    u16 port = 8080
    i64 wide = port // on its own: every u16 fits in an i64
    i64 ttl = 64
    u8 byte = u8(ttl) // written out: an i64 may not fit
    u64 sent = u64(wide) * 1000
    print("{wide} {byte} {sent}")
}
```

```output
8080 64 8080000
```

A conversion that can lose the value is checked: in a debug build, `u8(300)` with a value known only when the program runs stops it with a message. A signed value never converts on its own to an unsigned type, since a negative number has no unsigned value.

The two sides of an operator must have the same type, or one must convert on its own to the other. `i32 + u32` doesn't compile: neither holds all the values of the other, so the program says which one it means.

```iris error
i32 offset = -5
u32 seq = 1000
next := seq + offset // error: '+' can't mix u32 and i32; convert one side
```

## Comparing

`==`, `!=`, `<`, `<=`, `>` and `>=` compare two integers and give a `bool`. They follow the same rule as arithmetic: both sides of one type, or one that converts to the other.

```iris run
void main() {
    u8 ttl = 200
    i64 limit = 100
    print("{ttl > limit} {ttl == 200} {limit != 100}")
}
```

```output
true true false
```
