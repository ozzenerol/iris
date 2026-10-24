# Floating point

The two float types, their literals and printing, and how they mix with integers.

## f64 and f32

`f64` is a 64-bit float with about 15 significant digits. `f32` is half the size with about 7. Use `f64` by default. Use `f32` for large arrays and for C functions and file formats that need it.

A literal with a point or an exponent is an `f64`:

```iris run
void main() {
    rtt := 1.5       // milliseconds
    link := 2e10     // bits per second
    jitter := 2.5e-3 // seconds
    f32 loss = 0.1   // 10% of packets

    print("{rtt} {link} {jitter} {loss}")
}
```

```output
1.5 20000000000 0.0025 0.1
```

The point needs a digit on both sides: `0.5` and `2.0`.

## Printing

A float prints in the fewest digits that read back as the same number. `0.1` prints as `0.1`, though the stored value is slightly off. Very large and very small numbers use an exponent.

```iris run
void main() {
    print("{0.1 + 0.2} {1.0 / 3}")
    print("{1e21} {1e-7} {100.0} {-2.5}")
}
```

```output
0.30000000000000004 0.3333333333333333
1e+21 1e-7 100 -2.5
```

## Arithmetic

`+`, `-`, `*` and `/` work as on integers. `%` returns the remainder with its fraction.

```iris run
void main() {
    elapsed := 7.5 // seconds since start
    window := 2.0  // seconds per rate window

    print("{elapsed / window} windows, {elapsed % window}s into the last one")
    print("{elapsed + 1} {elapsed * 2}")
}
```

```output
3.75 windows, 1.5s into the last one
8.5 15
```

An integer literal next to a float becomes a float, so `elapsed + 1` works. An integer variable must be converted:

```iris error
i64 bytes = 3000
f64 seconds = 1.5
print("{bytes / seconds}") // error: '/' can't mix i64 and f64; convert one side, for example f64(...)
```

Float division by zero doesn't stop the program. It gives infinity, or NaN for `0.0 / 0.0`. NaN equals nothing, not even itself.

```iris run
import math

void main() {
    seconds := 0.0
    print("{1500.0 / seconds} {-1500.0 / seconds} {seconds / seconds}")

    rate := 0.0 / seconds
    print("{rate == rate} {math.isNaN(rate)}")
}
```

```output
inf -inf nan
false true
```

## Converting

`f32` converts to `f64` implicitly. Conversions between integers and floats are explicit:

```iris run
void main() {
    totalMs := 17
    pings := 4

    average := f64(totalMs) / f64(pings)
    print("average rtt {average}ms")
    print("{i64(2.9)} {i64(-2.9)} {u8(65.7)}")
}
```

```output
average rtt 4.25ms
2 -2 65
```

Float to integer truncates toward zero. Out-of-range values, infinity and NaN stop a debug build.

## The math module

`math` has the common functions and constants, all on `f64`:

```iris run
import math

void main() {
    // TCP's smoothed round trip: 7/8 old value, 1/8 new sample
    srtt := 0.875 * 100.0 + 0.125 * 140.0
    print("srtt {srtt}ms")

    print("{math.ceil(4000.0 / 1460)} segments for 4000 bytes")
    print("{math.log2(65536)} bits for a port, {math.pow(2, 10)} {math.sqrt(2)}")
    print("{math.floor(2.7)} {math.round(2.5)} {math.trunc(-2.7)}")
    print("{math.abs(-3)} {math.min(4, 9)} {math.max(4, 9)}")
}
```

```output
srtt 105ms
3 segments for 4000 bytes
16 bits for a port, 1024 1.4142135623730951
2 3 -2
3 4 9
```

The [math](../std/index.html#math) page lists everything. `str.parseFloat` reads a float from text (see [Text](06-text.html)).
