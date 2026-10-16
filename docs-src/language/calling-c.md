# Calling C

Iris works directly with C: it can call C functions, be called from C, and drop down to assembly.

`import c` is planned for 0.8. Until then, only the standard library reaches C.

## Importing a C header

`import c` reads a C header and makes its functions, types and constants available under `c.`:

```iris
import c "sqlite3.h"

!void open(str path) {
    c.sqlite3 *db
    if c.sqlite3_open(path, &db) != c.SQLITE_OK {
        return error(DbError.open, "can't open {path}")
    }
}
```

The library is linked with `iris build --link sqlite3`, or with a `link` line in `iris.mod`.

## Types

Iris types have the same size and layout as their C equivalents:

| C | Iris |
|---|---|
| `int8_t` ... `int64_t` | `i8` ... `i64` |
| `uint8_t` ... `uint64_t` | `u8` ... `u64` |
| `float`, `double` | `f32`, `f64` |
| `char *` | `u8 *` |
| `struct` | `struct` with the same fields |

A `str` passed where C expects `char *` is copied to a temporary string that ends with a zero byte. `str.fromC(p)` turns a C string back into a `str`.

## Called from C

`@export` makes an Iris function callable from C with its own name:

```iris
@export
i32 onRow(void *ctx, i32 n, u8 **values, u8 **names) {
    return 0
}
```

An exported function can't return `!T`, since C has no way to receive it.

## Assembly

`asm` holds assembly for the target machine. The last value in `rax` (or `x0` on arm64) is the result:

```iris
u64 cycles() {
    asm {
        rdtsc
        shl rdx, 32
        or rax, rdx
    }
}
```

## SIMD

Vector types hold several numbers and do the same operation on all of them at once:

```iris
u8x16 a = load(u8x16, data)
mask := a == u8x16.splat('\n')
```

The types are `u8x16`, `i32x4`, `f32x4`, `f64x2` and their 256-bit versions.
