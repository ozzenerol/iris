# Integers

The integer types, their literals, arithmetic, and conversion between them.

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

Use `i64` by default. It is the type of untyped numbers, lengths and indexes. `u8` is a byte, and text is made of bytes. The other types fit fixed layouts such as file formats, network protocols and C functions.

Each type has `min` and `max`:

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

Numbers can be decimal, hexadecimal (`0x`), binary (`0b`) or octal (`0o`). `_` between digits groups them and is ignored:

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

A character in single quotes is its byte value, a `u8`:

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

Escapes: `\n` newline, `\t` tab, `\r` carriage return, `\0` zero, `\\`, `\'`, `\"`, and `\x41` for a byte in two hex digits.

A literal takes its type from where it is used: a typed variable, a parameter, or the other side of an operator. Otherwise it is an `i64`. It must fit the type.

```iris run
void setTtl(u8 ttl) {
    print("ttl {ttl}")
}

void main() {
    u16 port = 8080
    i32 offset = -40

    setTtl(64)
    print("{port + 1} {offset * 2}")
}
```

```output
ttl 64
8081 -80
```

`setTtl(256)` is a compile error.

## Units

A number can carry a unit. `ns`, `us`, `ms`, `s`, `m` and `h` make a `Duration`, a count of nanoseconds. `KiB`, `MiB` and `GiB` multiply a plain integer by 1024, 1024² and 1024³.

```iris run
void main() {
    timeout := 90s
    poll := 250ms
    buf := 64KiB

    print("{timeout} {poll} {i64(poll)}")
    print("{buf} bytes")
}
```

```output
1m30s 250ms 250000000
65536 bytes
```

A literal has one unit, so a minute and a half is `90s` or `1m + 30s`. A `Duration` prints in units and `i64(d)` gives nanoseconds. Durations add and subtract, and multiply or divide by plain numbers. One duration divided by another gives a plain number:

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

The [time](../std/index.html#time) module has clocks, dates and times of day.

## Arithmetic

`+`, `-`, `*`, `/` and `%` take two values of one integer type and return that type. `/` truncates toward zero. `%` takes the sign of the left side.

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

Bit operators: `&`, `|`, `^` (xor), `~` (not), `<<` and `>>`. Right shift of a signed value keeps the sign.

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

A result that doesn't fit its type overflows. A debug build, which `iris run` makes, stops with a message showing where:

```iris
u8 ttl = 250
next := ttl + 10 // panic: integer overflow
```

Release builds don't check overflow:

- Unsigned values wrap: `250 + 10` in a `u8` is `4`.
- Signed values wrap in `release-safe`.
- `release-fast` assumes signed math never overflows and optimizes on that. A signed overflow there gives an undefined result.

Division by zero stops debug and `release-safe` builds. In `release-fast` it is unchecked and the result is undefined.

Arithmetic happens in the operands' type. Two `u8` values add as a `u8` even if the result goes into an `i64`. Convert first when the result needs more room:

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

The `math` module has explicit behaviour that holds in every build:

- `addWrap`, `mulWrap` wrap around, as hashes need.
- `addSat`, `mulSat` clamp at the `i64` limits.
- `addChecked`, `mulChecked` return the result and whether it fit.

```iris run
import math

void main() {
    total, ok := math.addChecked(9223372036854775800, 10)
    print("{ok} {math.addSat(9223372036854775800, 10)} {math.mulWrap(4294967296, 4294967296)}")

    print("{math.gcd(84, 36)} {math.isqrt(99)} {math.ipow(3, 4)} {math.divFloor(-7, 2)} {math.mod(-7, 3)}")
}
```

```output
false 9223372036854775807 0
12 9 81 -4 2
```

`math.divFloor` and `math.mod` round down, which calendars and ring buffers need: `-7 / 2` is `-3` but `math.divFloor(-7, 2)` is `-4`. For numbers beyond `i64`, use [big](../std/index.html#big).

## Converting

A value converts implicitly when the target type holds every value of the source: `u8` to `u16`, `i32` to `i64`, `u32` to `i64`. Other conversions use the type name like a function:

```iris run
void main() {
    u16 port = 8080
    i64 wide = port // implicit: every u16 fits in an i64

    i64 ttl = 64
    u8 byte = u8(ttl) // explicit: an i64 may not fit

    u64 sent = u64(wide) * 1000
    print("{wide} {byte} {sent}")
}
```

```output
8080 64 8080000
```

A narrowing conversion is checked: in a debug build, `u8(300)` at run time stops the program. Signed values never convert implicitly to unsigned types.

Both sides of an operator must have the same type, or one must convert implicitly to the other. `i32 + u32` is an error because neither holds all values of the other:

```iris error
i32 offset = -5
u32 seq = 1000
next := seq + offset // error: '+' can't mix u32 and i32; convert one side
```

## Comparing

`==`, `!=`, `<`, `<=`, `>` and `>=` return a `bool` and follow the same type rule as arithmetic:

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
