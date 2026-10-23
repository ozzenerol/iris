# Compile time

Work the compiler does while it builds the program: constants, values worked out by running code before the program exists, and code that is kept or left out by the platform it is built for. There is no preprocessor and no macros; all of this is ordinary Iris.

## const

A `const` is a value worked out when compiling, from numbers, text, other constants and operators on them:

```iris run
const KiB = 1024
const MAX_FRAME = 16 * KiB
const HEADER = 9
const MAX_PAYLOAD = MAX_FRAME - HEADER
const NAME = "edge"

void main() {
    print("{NAME}: frames up to {MAX_FRAME} bytes, {MAX_PAYLOAD} of payload")
}
```

```output
edge: frames up to 16384 bytes, 16375 of payload
```

Floating point constants work the same way, with `+`, `-`, `*`, `/` and `%` on numbers and other constants, including those of other modules such as `math.pi`:

```iris run
import math

const f64 SOLAR_MASS = 4.0 * math.pi * math.pi
const f64 DAYS_PER_YEAR = 365.24
const f64 TURN = 2 * math.pi

void main() {
    print("{SOLAR_MASS} {DAYS_PER_YEAR} {TURN}")
}
```

```output
39.47841760435743 365.24 6.283185307179586
```

Text with values in braces can be a constant too, when every value is a constant number, `bool` or text. The text is put together when compiling, and the constant is a `str`. The text can be written as it is, or given to `format` or `str.format`:

```iris run
import str

const HOST = "api.example.com"
const VERSION = 2
const BASE = str.format("https://{HOST}/v{VERSION}")
const AGENT = "edge/{VERSION} (+https://{HOST})"

void main() {
    print(BASE)
    print(AGENT)
}
```

```output
https://api.example.com/v2
edge/2 (+https://api.example.com)
```

A `Duration`, an enum or a struct can't go into a constant's text; a `comptime` constant can build that text (see below).

A constant has no address and takes no memory: each use is the value itself. It can be used where a value must be known when compiling: an array's size, a parameter's default, a field's default, a global's first value, a `match` arm.

```iris run
const WORKERS = 4

i64[WORKERS] busy // jobs in hand, for each worker

void main() {
    busy[WORKERS - 1] = 7
    print("{busy.len} workers, the last has {busy[3]} jobs")
}
```

```output
4 workers, the last has 7 jobs
```

## comptime

`comptime` runs a function while compiling and puts its result into the program, as a constant. The function is ordinary Iris, with loops, `List`s and the standard library:

```iris run
u32[256] makeCrcTable() {
    u32[256] table
    for i in 0..256 {
        u32 c = u32(i)
        for _ in 0..8 {
            c = if (c & 1) == 1 { 0xedb88320 ^ (c >> 1) } else { c >> 1 }
        }
        table[i] = c
    }
    return table
}

const CRC_TABLE = comptime makeCrcTable()

u32 crc32(str data) {
    u32 c = 0xffffffff
    for b in data {
        c = CRC_TABLE[(c ^ u32(b)) & 0xff] ^ (c >> 8)
    }
    return c ^ 0xffffffff
}

void main() {
    print("{crc32("hello")}")
}
```

```output
907060870
```

The table is worked out once, when building, and is part of the program's read-only memory: running the program costs nothing for it. `comptime` goes in a `const` at the top of a file, and takes the whole expression after it.

What it can give:

| Value | Becomes |
|---|---|
| a number, `bool`, `str`, `String` or a plain enum | a constant like one written out (a `String` becomes a `str`) |
| an array, or a `@json` struct, of those | a global that can't be changed |

Pointers and slices can't come out of `comptime`, since what they point to stays behind in the compiler. What the code prints shows while compiling, after `comptime:`, and a panic in it stops the build with its message.

The compiler builds the code it needs into a small program, runs it on the machine that builds, and reads the values back. A comptime constant can be used by the program and by other constants, but not by the code of another comptime constant.

## when

`when` keeps code only when a condition known when compiling is true. The other branches are not checked or compiled, so they can use names that exist only on another platform:

```iris run
when os == .linux {
    const OS_NAME = "Linux"
} else when os == .macos {
    const OS_NAME = "macOS"
}

void main() {
    when mode == .debug {
        print("a debug build")
    } else {
        print("a release build")
    }
    when arch == .x86_64 || arch == .arm64 {
        print("64-bit, on {if OS_NAME.len > 0 { "a known system" } else { "?" }}")
    }
}
```

```output
a debug build
64-bit, on a known system
```

`when` works around statements inside functions, and around declarations at the top of a file. It can test three names:

| Name | Values |
|---|---|
| `os` | `.linux`, `.macos` |
| `arch` | `.x86_64`, `.arm64` |
| `mode` | `.debug`, `.releaseSafe`, `.releaseFast` |

Conditions join with `&&`, `||` and `!`, and compare with `==` and `!=`. The values are those of the platform the program is built for, which `--target` changes (see [Tools](39-tools.html)).

## Attributes

Attributes, written with `@` before a declaration, are the other part of compile-time work: they make the compiler write code for the program. The next chapter covers them.
