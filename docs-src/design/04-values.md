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

`String` is text the program owns: a pointer to its bytes, how many there are, and how many fit before it must grow. Its memory comes from `malloc`, and `free` gives it back.

```output
String  =  { u8 *ptr, i64 len, i64 cap }
```

## Arrays

`u8[512]` is 512 bytes in a row, with no length stored. When an array is passed to a function, it becomes a slice.

## Structs

Fields are stored in the order they are written, with the same padding as C. A struct can be passed to and from C as it is. `packed` removes the padding and `align(n)` raises the alignment. A run of bitfields shares one integer of their type, the first in its highest bits. `u16be`, `u32be` and `u64be` (and the signed ones) are only field types: they read as plain integers and keep the bytes in network order.

## Enums

A plain enum is a 32-bit integer, like a C enum.

An enum whose values carry data is a tag followed by room for the largest value's data:

```output
Packet  =  { i32 tag, union { u8[] bytes, u16 code } }
```

In LLVM IR the room is an array of 8-byte words, `{ i32, [N x i64] }`, and each value's data is a struct of its fields stored in that room. The compiler works out the sizes with the same layout rules as C.

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

Each generic function and struct is compiled once for each set of types it is used with. `max` called with `Version` values and with `Date` values gives two separate functions in the program, `max[Version]` and `max[Date]`. Calls are direct, with no lookup at run time.

## Interfaces and dyn

An interface used as a constraint (`T: Ordered`) is only checked by the compiler. It leaves nothing in the program.

A `dyn Writer` value is two pointers: one to the data, and one to a table with the type's functions for that interface. A call through `dyn` reads the function from the table.

```output
dyn Writer  =  { void *data, WriterTable *table }
```

## String interpolation

`print("got {n} bytes")` becomes a write of `"got "`, a write of `n`, and a write of `" bytes"`, into the output buffer. Nothing is allocated.

## Tasks

Each task has its own stack, 256 KiB with a guard page below it, so running past it crashes instead of corrupting memory. A scheduler in the runtime runs the tasks on top of an event loop. On Linux the loop uses io_uring: reads and accepts are handed to the kernel, and one system call per turn submits them all and collects the results. Where io_uring is missing or blocked, as in many containers, the loop uses epoll, and on macOS it uses kqueue. `IRIS_IO=epoll` picks epoll on Linux. Each file descriptor is added to epoll or kqueue once and stays there until it is closed. Finished tasks keep their stacks for the next spawns, up to 16 per thread. When a task waits for I/O, a channel or a timer, the scheduler saves its registers and switches to another task, with a few instructions of assembly per machine (x86_64 and arm64). When the wait is over, the task continues where it stopped.

`spawn f(a, b)` stores the arguments in memory and starts a task with a small function the compiler writes, which reads them back, makes the call and frees them. `thread f(a, b)` does the same on a new thread.

Each thread has its own scheduler. A task that another thread wakes, for example by sending to a channel it waits on, goes into its thread's inbox, and a write to the thread's wake descriptor (an eventfd on Linux, a pipe on macOS) interrupts its event loop. Channels and `Mutex` keep a short spin lock. A task waiting in `select` on several channels can be woken by any of them, so each waker first claims the task; only the first claim wakes it.

`Atomic[T]` methods become LLVM atomic instructions. A `Mutex` is 24 bytes: a spin lock, a flag, and the list of waiting tasks.

A channel is a ring buffer of values with lists of the tasks waiting to put into it or take out of it. `select` puts its task on the list of every channel it waits on, and on the timer list for `after`.
