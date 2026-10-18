# Calling C

Iris calls C functions directly, with no wrapper code to write: `import c` reads a C header and makes its functions, types and constants available. C can also call Iris functions back. This chapter covers both, how C types look in Iris, and the rules for text and memory at the border.

## import c

`import c "header.h"` reads the header with `clang` and makes what it declares available under `c.`:

```iris run
import c "arpa/inet.h", c "stdio.h", c "string.h"

void main() {
    c.puts("parsing an address\0".ptr)
    u8[4] addr
    ok := c.inet_pton(c.AF_INET, "192.168.1.10\0".ptr, &addr[0])
    print("{ok}: {addr[0]} {addr[1]} {addr[2]} {addr[3]}")
    n := c.strlen("192.168.1.10\0".ptr)
    c.printf("%s has %ld characters\n\0".ptr, "192.168.1.10\0".ptr, n)
}
```

```output
parsing an address
1: 192 168 1 10
192.168.1.10 has 12 characters
```

`inet_pton` writes the four bytes of the address into `addr`, and `c.AF_INET` is the constant from the header.

All the headers a file imports share one `c`. Headers are found where `clang` looks for them, in the project's folder, and in folders given with `-I`. The C library is always linked; another library is linked with `-l name` on the command line, or a `link` line in `iris.mod`:

```shell
iris run -l sqlite3 main.iris
```

```output
module github.com/me/app

link sqlite3
```

`iris c header.h` prints what Iris makes of a header: each function, struct, type and constant, and what it left out and why.

## Text at the border

C text ends with a zero byte, and Iris text doesn't: a `str` knows its length instead. Text given to C needs the zero added, and `.ptr` gives the pointer: `"name\0".ptr`, or a `String` with `"\0"` appended. Text that comes back from C is read with a slice of the pointer, up to its zero:

```iris run
import c "stdlib.h", c "string.h"

void main() {
    u8 *home = c.getenv("HOME\0".ptr)
    if home != null {
        s := home[0:c.strlen(home)]
        print("HOME is set: {s.len > 0}")
    }
}
```

```output
HOME is set: true
```

The slice points into C's memory, and is valid as long as C keeps it there.

## Types

C types become Iris types of the same size and layout, so values pass between the two as they are:

| C | Iris |
|---|---|
| `char`, `unsigned char`, `signed char` | `u8`, `u8`, `i8` |
| `short`, `int`, `long`, `long long` | `i16`, `i32`, `i64`, `i64` |
| the `unsigned` ones | `u16`, `u32`, `u64`, `u64` |
| `float`, `double` | `f32`, `f64` |
| `T *`, `char *`, `void *` | `T *`, `u8 *`, `u8 *` |
| a pointer to a function | a function type: `int (*)(const void *, const void *)` is `i32(u8 *, u8 *)` |
| `struct` | a struct with the same fields; one only used through pointers has none |
| `enum` | its values, as `i32` constants |
| `typedef` | a type alias |

A C name that is an Iris keyword or built-in name gets a `_` after it: `c.free_`, `c.select_`. A struct named like a function, as `struct stat` and `stat()` are, is `c.struct_stat`. Functions with `...`, like `printf`, take extra numbers and pointers.

Left out, for now, with the reason shown by `iris c`: `long double`, unions, structs passed or returned by value, inline functions, and macros that aren't plain numbers, text, or the name of another such macro.

## Callbacks

Where C takes a pointer to a function, an Iris function with the same types can be passed. C's `void *` arguments arrive as `u8 *`, which converts to the pointer type that is meant:

```iris run
import c "stdlib.h"

i32 compare(u8 *pa, u8 *pb) {
    i64 *a = pa
    i64 *b = pb
    return if *a < *b { -1 } else if *a > *b { 1 } else { 0 }
}

void main() {
    i64[5] rtts = [52, 31, 90, 12, 70] // milliseconds
    c.qsort(&rtts[0], 5, 8, compare)
    print("{rtts[0]} {rtts[1]} {rtts[2]} {rtts[3]} {rtts[4]}")
}
```

```output
12 31 52 70 90
```

## Called from C

`@export` makes an Iris function callable from C by its own name, for C code that links with the program or a library that looks callbacks up by name. `@export("name")` gives it another C name:

```iris
@export
i32 onRow(u8 *ctx, i32 n, u8 **values, u8 **names) {
    return 0
}
```

An exported function takes and gives numbers and pointers, never `!T`, since C has no way to take an error. The names `main` and those starting with `iris_` belong to the runtime.

## Memory at the border

Memory from C's `malloc` is freed with C's `free` (`c.free_`), and memory from Iris's `alloc` with Iris's `free`; the two don't mix. A C function that keeps a pointer it was given expects the memory to stay: give it memory from `alloc`, or a global, not a local variable's address.

## The standard library and C

Parts of the standard library are made this way: TLS calls OpenSSL, `sqlite` calls SQLite, and `compress` calls zlib, each loaded the first time a program uses it. A program that needs another C library uses `import c` the same way.
