# Compile time

Work that the compiler does while building the program: constants, code for one platform, and code generated from attributes. Iris has no preprocessor and no macros.

## const

A `const` is worked out when compiling. It can use other constants and simple expressions:

```iris
const KiB = 1024
const BUF_SIZE = 64 * KiB
const NAME = "iris"
```

## comptime

`comptime` runs a function while compiling and puts the result in the program. The function is ordinary Iris code:

```iris
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
```

`comptime` goes in a `const` at the top of one of the program's files, and takes the whole expression after it. The compiler builds the code it needs into a small program, runs it on the machine that builds, and puts the values it gives into the real program. Running the result has no cost: `CRC_TABLE` is a table in the program's read-only memory.

What comptime can give:

| Value | Becomes |
|---|---|
| a number, `bool`, `str`, `String` or a plain enum | a constant, like one written out (a `String` becomes a `str`) |
| an array or a `@json` struct of those | a global that can't change |

Pointers and slices can't come out of comptime, since the memory they point to stays behind. What the code prints shows up while compiling, after `comptime:`, and a panic in it stops the build with its message.

A comptime constant can be used by other constants and by the program, but not yet by the code of another comptime constant: while the helper runs, comptime constants are zeros. Comptime code runs on the machine that builds, also with `--target`.

## when

`when` includes code only when a condition known at compile time is true. The other branches are not checked or compiled, so they can use names that exist only on another platform:

```iris
when os == .linux {
    poller := epoll()
} else when os == .macos {
    poller := kqueue()
}
```

`when` can test three names, against these values:

| Name | Values |
|---|---|
| `os` | `.linux`, `.macos` |
| `arch` | `.x86_64`, `.arm64` |
| `mode` | `.debug`, `.releaseSafe`, `.releaseFast` |

Conditions can be joined with `&&`, `||` and `!`, and compared with `==` or `!=`. The values are those of the platform the program is built for, which `--target` can change.

`when` also works outside functions, around declarations:

```iris
when os == .macos {
    const LIB = "libsqlite3.dylib"
} else {
    const LIB = "libsqlite3.so"
}
```

## Attributes

An attribute starts with `@` and goes before a declaration. Some attributes make the compiler generate code.

`@json` lets a struct be turned into JSON and read back, with the `json` module:

```iris
import json

@json
struct User {
    i64 id
    String name
    @json(required) String email
    @json("created_at") i64 created
    bool active = true
    String[] tags
}

scoped User u = try json.decode(User, body)
scoped String out = json.encode(u)
```

`json.encode(v)` gives the JSON text as a `String`. `json.decode(T, text)` reads a `T` from JSON text and gives a `!T`.

A field's JSON key is its name as written. `@json("created_at")` gives it another key, and `@json(required)` makes `json.decode` fail when the key is missing. Both can go together: `@json("id", required)`. Any other missing field keeps its default value, and keys the struct doesn't have are skipped. `null` leaves a field as it is.

A `@json` struct can hold integers, floats, `bool`, `String`, plain enums (written as their value's name), other `@json` structs, and slices and arrays of those. Fields of an embedded struct appear as fields of the outer one. A `str` field can be encoded but not decoded, since a `str` can't own its text.

A `@json` struct that has no `free` method gets one, which frees its Strings and slices, so `scoped` works on decoded values. When `json.decode` fails, what it had read is freed already.

The errors are values of `json.JsonError`: `syntax` (the text isn't JSON), `wrongType` (a value has the wrong type), `missing` (a required field) and `range` (a number doesn't fit). The message says where:

```output
tags[0]: expected a string, found a number
home.zip: 70000 doesn't fit in u16
line 1, column 15: expected a key in quotes
```

`@env` will read a struct's fields from environment variables (planned):

```iris
@env
struct Settings {
    @env("PORT") u16 port = 8080
    @env("DATABASE_URL", required) String db
}

cfg := try env.load(Settings)
```

Other attributes:

| Attribute | Effect |
|---|---|
| `@yaml` | like `@json`, for YAML (planned) |
| `@unchecked` | no bounds or overflow checks in this function (planned) |
| `@export` | the function can be called from C |
| `@inline` | always inline this function (planned) |
