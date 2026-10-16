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

## when

`when` includes code only when a condition known at compile time is true. The other branches are not compiled:

```iris
when os == .linux {
    poller := epoll()
} else when os == .macos {
    poller := kqueue()
}
```

The names that `when` can test are `os`, `arch` and `mode` (debug, release-safe or release-fast).

## Attributes

An attribute starts with `@` and goes before a declaration. Some attributes make the compiler generate code.

`@json` generates the code that turns a struct into JSON and back:

```iris
@json
struct User {
    i64 id
    String name
    @json("created_at") i64 created
}

User u = try json.decode(User, body)
scoped String out = json.encode(u)
```

`@env` reads a struct's fields from environment variables:

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
| `@yaml` | like `@json`, for YAML |
| `@unchecked` | no bounds or overflow checks in this function |
| `@export` | the function can be called from C |
| `@inline` | always inline this function |
