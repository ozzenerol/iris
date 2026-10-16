# Calling C

Iris works directly with C: it can call C functions, be called from C, and drop down to assembly.

## Importing a C header

`import c` reads C headers with `clang` and makes their functions, structs, types and integer and string constants available under `c.`:

```iris run
import c "stdio.h", c "string.h"

void main() {
    c.puts("hello from C\0".ptr)
    n := c.strlen("iris\0".ptr)
    c.printf("%s has %ld letters\n\0".ptr, "iris\0".ptr, n)
}
```

```output
hello from C
iris has 4 letters
```

All the headers a file imports share one `c`. Headers are found where `clang` looks for them, in the project's root, and in folders given with `-I`.

A library is linked with `-l`, or with a `link` line in `iris.mod`:

```shell
iris run -l sqlite3 main.iris
```

```output
module github.com/me/app

link sqlite3
```

```iris
import c "sqlite3.h"

!void open(str path) {
    c.sqlite3 *db
    if c.sqlite3_open(path.ptr, &db) != c.SQLITE_OK {
        return error(DbError.open, "can't open {path}")
    }
}
```

`iris c header.h` prints what Iris makes of a header, and what it leaves out and why.

## Text

C expects text to end with a zero byte, and Iris text doesn't. Pass `.ptr` of text that ends with `\0`, as in the examples above. A C string can be read back with a slice of the pointer: `p[0:c.strlen(p)]`.

## Types

C types become Iris types of the same size and layout:

| C | Iris |
|---|---|
| `char`, `unsigned char`, `signed char` | `u8`, `u8`, `i8` |
| `short`, `int`, `long`, `long long` | `i16`, `i32`, `i64`, `i64` |
| `unsigned` versions | `u16`, `u32`, `u64`, `u64` |
| `T *`, `char *` | `T *`, `u8 *` |
| `void *` | `u8 *` |
| a pointer to a function | `u8 *` |
| `struct` | a `struct` with the same fields, or an empty one when only pointers to it make sense |
| `enum` | its values as `i32` constants |
| `typedef` | a `type` alias |

A C name that is an Iris keyword or built-in name gets a `_` after it: `c.free_`, `c.select_`. A struct that shares its name with a function, like `struct stat` and `stat()`, is `c.struct_stat`.

Functions with `...`, like `printf`, take extra integers and pointers.

Left out for now, with the reason shown by `iris c`: floating point, unions, structs passed or returned by value, inline functions, and macros that aren't plain numbers or strings.

## Called from C

`@export` makes an Iris function callable from C with its own name:

```iris
@export
i32 onRow(u8 *ctx, i32 n, u8 **values, u8 **names) {
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
