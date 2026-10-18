# Floating point

Numbers with a fraction: the two types, how they are written and printed, and how they mix with integers.

## f64 and f32

`f64` is a 64-bit floating point number, with about 15 significant digits. `f32` takes half the room and has about 7. `f64` is the one to use; `f32` is for large arrays where the room matters, and for C functions and file formats that use it.

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

There must be a digit on each side of the point: `0.5`, not `.5`, and `2.0`, not `2.`.

## Printing

A float prints in the fewest digits that read back as the same number, so `0.1` prints as `0.1` even though the number stored is a little off from a tenth. Very large and very small numbers use an exponent:

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

`+`, `-`, `*` and `/` work as on integers. `%` gives what is left after dividing, with a fraction:

```iris run
void main() {
    elapsed := 7.5 // seconds since the server started
    window := 2.0  // its rate is counted per 2-second window
    print("{elapsed / window} windows, {elapsed % window}s into the last one")
    print("{elapsed + 1} {elapsed * 2}")
}
```

```output
3.75 windows, 1.5s into the last one
8.5 15
```

An integer literal used with a float becomes a float, so `elapsed + 1` needs no `1.0`. A variable of an integer type doesn't: the two types must be made the same.

```iris error
i64 bytes = 3000
f64 seconds = 1.5
print("{bytes / seconds}") // error: '/' can't mix i64 and f64; convert one side, for example f64(...)
```

Division by zero doesn't stop the program for floats. It gives infinity, or NaN ("not a number") for `0.0 / 0.0`, as the hardware does. NaN is not equal to anything, even itself:

```iris run
import math

void main() {
    seconds := 0.0 // nothing measured yet
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

`f32` converts to `f64` on its own. Integers and floats convert only when it is written out, with the type's name:

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

Converting a float to an integer drops the fraction, toward zero. A value that doesn't fit, infinity and NaN stop a debug build with a message, since there is no integer for them.

## The math module

`math` has the usual functions and constants. All of them take and give `f64`:

```iris run
import math

void main() {
    // the smoothed round trip time TCP keeps: 7/8 of the old value, 1/8 of the new sample
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

The [math](../std/index.html#math) page of the library lists them all. `str.parseFloat` reads a float from text (see [Text](06-text.html)).
