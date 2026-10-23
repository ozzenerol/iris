# Math and big numbers

The `math` module has the functions of floating point math, such as roots, powers, logarithms and trigonometry, and careful integer helpers. [Integers](../tour/03-integers.html) and [Floating point](../tour/04-floating-point.html) show the basics. This chapter puts them to work, and then turns to the `big` module, for whole numbers too large for an `i64`.

## Distances

Trigonometry works in radians, so degrees are multiplied by `math.pi / 180` first. The distance between two places on Earth, from their latitude and longitude, uses `sin`, `cos`, `atan2` and `sqrt`:

```iris run
import math

struct Place {
    str name
    f64 lat
    f64 lon
}

const f64 EARTH_KM = 6371.0

f64 radians(f64 deg) => deg * math.pi / 180

// The distance along the surface, in kilometres (the haversine formula).
f64 distance(Place a, Place b) {
    dlat := radians(b.lat - a.lat)
    dlon := radians(b.lon - a.lon)
    h := math.pow(math.sin(dlat / 2), 2) + math.cos(radians(a.lat)) * math.cos(radians(b.lat)) * math.pow(math.sin(dlon / 2), 2)
    return 2 * EARTH_KM * math.atan2(math.sqrt(h), math.sqrt(1 - h))
}

void main() {
    frankfurt := Place{name: "Frankfurt", lat: 50.11, lon: 8.68}
    for p in [Place{name: "Amsterdam", lat: 52.37, lon: 4.90}, Place{name: "Virginia", lat: 38.95, lon: -77.45}, Place{name: "Singapore", lat: 1.35, lon: 103.82}] {
        print("{p.name}: {math.round(distance(frankfurt, p))} km")
    }
}
```

```output
Amsterdam: 364 km
Virginia: 6553 km
Singapore: 10258 km
```

`math.hypot(x, y)` gives the length of a line from its two sides, for distances on a flat map or a screen.

## Statistics

The mean and the standard deviation of a set of measurements take a sum and a square root. Percentiles, such as the p99 of response times, take the values in order:

```iris run
import math

bool less(f64 a, f64 b) => a < b

// The value below which p percent of the sorted values fall.
f64 percentile(List[f64] *sorted, f64 p) {
    i := i64(math.ceil(p / 100 * f64(sorted.len))) - 1
    return sorted.at(math.clamp(i, 0, sorted.len - 1))
}

void main() {
    scoped List[f64] ms
    for v in [12.0, 15.5, 11.2, 13.8, 250.0, 14.1, 12.9, 16.3, 13.0, 12.4] {
        ms.push(v)
    }
    f64 sum = 0
    for v in ms {
        sum += v
    }
    mean := sum / f64(ms.len)
    f64 sq = 0
    for v in ms {
        sq += (v - mean) * (v - mean)
    }
    sd := math.sqrt(sq / f64(ms.len))
    ms.sort(less)
    print("mean {mean:.1} ms, deviation {sd:.1} ms")
    print("p50 {percentile(&ms, 50)} ms, p90 {percentile(&ms, 90)} ms, p99 {percentile(&ms, 99)} ms")
}
```

```output
mean 37.1 ms, deviation 71.0 ms
p50 13 ms, p90 16.3 ms, p99 250 ms
```

One slow request pulled the mean far above what most requests took, which is why services watch percentiles. `math.clamp(x, lo, hi)` keeps a value between two ends, for any type that compares.

## Growth and logarithms

`math.pow` and `math.exp` give growth, and `math.log`, `log2` and `log10` turn it around, to say how long growth takes or how many steps a search needs:

```iris run
import math

void main() {
    // traffic that grows 8% a month
    rate := 1.08
    print("in a year: {math.pow(rate, 12):.2} times as much")
    print("twice as much after {math.ceil(math.log(2) / math.log(rate))} months")
    // a binary search over a million sorted keys
    print("{math.ceil(math.log2(1000000))} steps at most")
    // retries that wait twice as long each time, up to 30 seconds
    for attempt in 0..7 {
        wait := math.min(0.1 * math.pow(2, f64(attempt)), 30)
        print("attempt {attempt}: wait {wait:.1}s")
    }
}
```

```output
in a year: 2.52 times as much
twice as much after 10 months
20 steps at most
attempt 0: wait 0.1s
attempt 1: wait 0.2s
attempt 2: wait 0.4s
attempt 3: wait 0.8s
attempt 4: wait 1.6s
attempt 5: wait 3.2s
attempt 6: wait 6.4s
```

## Integers that must not overflow

An `i64` holds numbers up to about 9.2 × 10¹⁸. Arithmetic past that panics in the debug build (see [Integers](../tour/03-integers.html)). Where a value comes from outside and could be that large, `math.addChecked`, `subChecked` and `mulChecked` give the result and whether it fit, and `addSat`, `subSat` and `mulSat` stop at the largest or smallest value instead:

```iris run
import math

void main() {
    bytes := 3000000000
    total, ok := math.mulChecked(bytes, 4000000000)
    print("fits: {ok}")
    quota := math.mulSat(bytes, 4000000000)
    print("saturated: {quota == 9223372036854775807}")
    print("{math.gcd(1920, 1080)} {math.lcm(4, 6)} {math.ipow(2, 40)} {math.isqrt(1000000)}")
}
```

```output
fits: false
saturated: true
120 12 1099511627776 1000
```

For numbers that are larger still, there is `big`.

## Whole numbers of any size

A `big.Int` holds a whole number of any size, limited only by memory. `big.from(n)` makes one from an `i64`, and `big.parse(text)` from text, in base 10 or any base from 2 to 36. Each operation gives a new `Int`, which owns memory and is freed after use, so `scoped` suits them. `{n}` writes one in decimal:

```iris run
import big

void main() {
    scoped big.Int f = big.from(1)
    for i in 2..31 {
        scoped big.Int k = big.from(i)
        next := f.mul(k)
        f.free()
        f = next
    }
    print("30! = {f}")
    scoped String hex = f.text(16)
    print("{f.bitLen()} bits, {hex} in hex")
}
```

```output
30! = 265252859812191058636308480000000
108 bits, d13f6370f96865df5dd54000000 in hex
```

The loop replaces `f` with each new product and frees the one before. `add`, `sub`, `mul`, `div`, `rem` and `divMod` do arithmetic, `pow` raises to an `i64` power, `cmp` and `equal` compare, and `shl` and `shr` shift.

## Ids and keys as numbers

Numbers too large for an `i64` turn up as ids, hashes and keys: a 128-bit id in hex, or a 256-bit balance on a blockchain. `big.parse(text, 16)` reads one, `_` may separate the digits, and `toI64` gives an `i64` back when the number fits, or fails with `BigError.tooBig`:

```iris run
import big

!void main() {
    scoped id := try big.parse("0190e5d4_8a2b_7c3d_9e4f_5a6b7c8d9e0f", 16)
    print("{id}")
    scoped String back = id.text(16)
    print(back)
    scoped top := id.shr(80)
    print("the first 48 bits: {try top.toI64()} ms")
    _ = id.toI64() catch e {
        print("{e.code}")
        0
    }
    _ = big.parse("12ab", 10) catch e {
        print("{e.code}")
        big.from(0)
    }
}
```

```output
2081580254345122183670600953221717519
190e5d48a2b7c3d9e4f5a6b7c8d9e0f
the first 48 bits: 1721842829867 ms
BigError.tooBig
BigError.invalid
```

`text(16)` writes the number without the zero it started with, since a number has no leading zeros. The first 48 bits of that UUID are the time it was made, in milliseconds since 1970 (see [Encodings and ids](11-encodings-and-ids.html#uuids)).

## Modular arithmetic

`modPow(e, m)` raises to a power modulo `m` without ever making the whole power, which would have more digits than there are atoms. It is the step at the heart of RSA and of many checks for primes. Here a small RSA key encrypts and decrypts a number:

```iris run
import big

!void main() {
    // two primes, kept secret, and their product, which is public
    scoped p := try big.parse("61")
    scoped q := try big.parse("53")
    scoped n := p.mul(q)
    scoped e := big.from(17)   // the public exponent
    scoped d := big.from(2753) // the private one: 17 * 2753 = 1 mod 60 * 52
    scoped msg := big.from(65)
    scoped secret := msg.modPow(e, n)
    scoped back := secret.modPow(d, n)
    print("n = {n}, {msg} -> {secret} -> {back}")
    scoped g := big.from(3120).gcd(e)
    print("gcd(3120, 17) = {g}")
}
```

```output
n = 3233, 65 -> 2790 -> 65
gcd(3120, 17) = 1
```

Real keys use primes of hundreds of digits, and this code works the same for them. To sign and encrypt in a program, use `crypto` (see [Random numbers and cryptography](19-random-and-crypto.html)), which does it safely; `big` is for the arithmetic itself.
