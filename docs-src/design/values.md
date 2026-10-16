# How values are stored

What each Iris type looks like in memory, and what the compiler turns each feature into.

## Numbers and bool

Integers and floats are stored as the machine stores them: `i32` is 4 bytes, `u64` is 8 bytes, `f64` is 8 bytes. `bool` is one byte, 0 or 1.

## Pointers

A pointer is a memory address, 8 bytes on 64-bit machines. It can be `null` (0).

## Slices and str

A slice `T[]` is two values: a pointer to the first item and the number of items.

```output
T[]  =  { T *ptr, i64 len }
```

`str` is `u8[]`. A string literal is a `str` that points into read-only memory. Slicing (`s[2:5]`) makes a new pointer and length and copies nothing.

## String

`String` is text the program owns. It remembers which allocator its memory came from, so `free` can give it back.

```output
String  =  { u8 *ptr, i64 len, i64 cap, Allocator *alloc }
```

Until allocators arrive, `String` is `{ u8 *ptr, i64 len, i64 cap }` and its memory comes from `malloc`.

## Arrays

`u8[512]` is 512 bytes in a row, with no length stored. When an array is passed to a function, it becomes a slice.

## Structs

Fields are stored in the order they are written, with the same padding as C. A struct can be passed to and from C as it is. `packed` removes the padding and `align(n)` sets the alignment.

## Enums

A plain enum is a 32-bit integer, like a C enum.

An enum whose values carry data is a tag followed by room for the largest value's data:

```output
Packet  =  { i32 tag, union { u8[] bytes, u16 code } }
```

## Several return values

`(i64, i64)` is a struct with two fields. A function that returns one returns that struct.

## Errors and !T

`Error` holds what went wrong:

```output
Error  =  { u32 kind, u32 code }
```

`kind` says which error enum the code belongs to, and `code` is the value in that enum. In the compiled program the two are one `i64`, with `kind` in the high half. An `Error` of zero means no error. The message and the trace are kept in a per-thread buffer, so an `Error` needs no memory of its own.

A new error's message is written to a second buffer and swapped in when the error is made, so a message can quote the error it replaces: `error(e.code, "loading user {id}: {e.message}")`.

`!T` is a `T` and an `Error` side by side:

```output
!i64  =  { i64 value, Error err }
```

`try f()` becomes a call followed by a check. If `err` is not zero, the current function adds its name and line to the trace and returns the error. When nothing fails, the cost is that one comparison.

## defer, errdefer and scoped

The compiler copies the code of a `defer` to every place where its block can end: the end of the block, each `return`, `break` and `continue`, and each `try` that can leave. Several defers run in reverse order. An `errdefer` is copied only to the places where the function returns an error.

`scoped String s = ...` is the same as declaring `s` and adding `defer s.free()`.

## Generics

Each generic function and struct is compiled once for each set of types it is used with. `max(i64, i64)` and `max(f64, f64)` are two separate functions in the program. Calls are direct, with no lookup at run time.

## Interfaces and dyn

An interface used as a constraint (`T: Ordered`) is only checked by the compiler. It leaves nothing in the program.

A `dyn Writer` value is two pointers: one to the data, and one to a table with the type's functions for that interface. A call through `dyn` reads the function from the table.

```output
dyn Writer  =  { void *data, WriterTable *table }
```

## String interpolation

`print("got {n} bytes")` becomes a write of `"got "`, a write of `n`, and a write of `" bytes"`, into the output buffer. Nothing is allocated.

## Tasks

Each task has its own stack. A scheduler runs the tasks on top of an event loop (epoll on Linux, kqueue on macOS, io_uring later). When a task waits for I/O, the scheduler saves its registers and switches to another task. When the I/O is ready, the task continues where it stopped.
