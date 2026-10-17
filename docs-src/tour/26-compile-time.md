# Compile time

Work the compiler does while it builds the program: constants, values worked out by running code before the program exists, and code that is kept or left out by the platform it is built for. There is no preprocessor and no macros; all of this is ordinary Iris.

## const

A `const` is a value worked out when compiling, from numbers, text, other constants and operators on them:

```iris run
const KiB = 1024
const BUFFER = 64 * KiB
const NAME = "iris"
const VERSION = 3
const WIDTH = VERSION * 10 + 2

void main() {
    print("{NAME} v{VERSION}: {BUFFER} bytes, width {WIDTH}")
}
```

```output
iris v3: 65536 bytes, width 32
```

A constant has no address and takes no memory: each use is the value itself. It can be used where a value must be known when compiling: an array's size, a parameter's default, a field's default, a global's first value, a `match` arm.

```iris run
const SLOTS = 4

i64[SLOTS] counts

void main() {
    counts[SLOTS - 1] = 7
    print("{counts.len} {counts[3]}")
}
```

```output
4 7
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

Conditions join with `&&`, `||` and `!`, and compare with `==` and `!=`. The values are those of the platform the program is built for, which `--target` changes (see [Tools](31-tools.html)).

## Attributes

Attributes, written with `@` before a declaration, are the other part of compile-time work: they make the compiler write code for the program. The next chapter covers them.
