# How values are stored

What each Iris type looks like in memory and in LLVM IR. `llType` in `src/ir.c` maps a `Type` to its IR type, and `typeSize` and `typeAlign` in `src/check.c` give its size and alignment.

## Numbers, bool and pointers

Integers and floats are stored as the machine stores them: `i32` is 4 bytes, `u64` 8 bytes, `f64` 8 bytes. LLVM integers carry no sign, so `i32` and `u32` are both `i32` in IR, and the IR writer picks signed or unsigned instructions. `bool` is `i1` in registers and one byte, 0 or 1, in memory.

A pointer is an address, 8 bytes on 64-bit machines, and `ptr` in IR. It can be `null` (0). Function values, channels and `Thread` handles are pointers too.

## Slices and str

A slice `T[]` is a pointer to the first item and the number of items:

```output
T[]  =  { ptr, i64 }
```

`str` is `u8[]`. A string literal is a `str` that points into read-only memory. Slicing (`s[2:5]`) makes a new pointer and length and copies nothing.

## String

`String` is text the program owns: a pointer to its bytes, how many there are, and how many fit before it must grow. Its memory comes from the runtime's allocator, and `free` gives it back. A `String` field's default text is borrowed with `cap` 0, so nothing is made or freed for it.

```output
%String  =  { ptr, i64 len, i64 cap }
```

A `String` or `List` doubles its room as it grows, and past 1 MiB grows by half, so a big one keeps less unused room.

## Arrays

`u8[512]` is 512 bytes in a row, `[512 x i8]` in IR, with no length stored. An array passed to a function becomes a slice.

## Structs

Fields are stored in the order they are written, with the same padding as C, so a struct can be passed to and from C as it is. `layoutStruct` writes the padding as explicit `[N x i8]` elements of the IR type. `packed` removes the padding and becomes `<{ ... }>`. `align(n)` raises the alignment.

A run of bitfields shares one integer of their type, the first field in its highest bits. `u16be`, `u32be` and `u64be` (and the signed ones) are only field types. They read as plain integers and keep the bytes in network order, with a byte swap on each read and write.

## Enums

A plain enum is a 32-bit integer, like a C enum. A flags enum is the same, with one bit per value.

An enum whose values carry data is a tag followed by room for the largest value's data:

```output
Packet  =  { i32 tag, [N x i64] }
```

The room is an array of 8-byte words. Each value's data is a struct of its fields, stored in that room. `Type.payload` holds the room's size in bytes.

## Several return values

`(i64, i64)` is a struct with two fields, `{ i64, i64 }`. A function that returns several values returns that struct.

## Errors and !T

An `Error` is one `i64`: the kind in the high 32 bits, which says which error enum the code belongs to (`Type.count` of the enum, from 1), and the value in that enum in the low 32 bits. An `Error` of zero means no error.

```output
Error  =  i64 (kind << 32 | code)
!T     =  { T, i64 }
!void  =  i64
```

The message and the trace are not part of the value. The runtime keeps them per thread, so an `Error` needs no memory of its own. See [The runtime](06-runtime.html#errors).

## dyn and interfaces

An interface used as a bound (`T: Ordered`) is only checked by the compiler. It leaves nothing in the program.

A `dyn Writer` is a pointer to the data and a pointer to a method table for that type and interface:

```output
dyn Writer  =  { ptr data, ptr table }
```

The table is a constant array of function pointers, one per method of the interface, written once per struct and interface (`vtable` in `src/ir.c`). A call through `dyn` loads the function from the table and calls it with the data pointer as `self`.

## Generics

Each generic function and struct is compiled once for each set of types it is used with. `max` called with `Version` values and with `Date` values gives two separate functions, `max[Version]` and `max[Date]`. Calls are direct, with no lookup at run time.

## Sync types

`Atomic[T]` has the layout of `T`, with `bool` as one byte, and its methods become LLVM atomic instructions, all sequentially consistent. A `Mutex` is 32 bytes, `[4 x i64]` in IR: a spin lock, a locked flag, the list of waiting tasks, and the thread that holds it. Neither can be copied.

## String interpolation

`print("got {n} bytes")` writes `"got "`, then `n`, then `" bytes"` straight to the output, with one runtime call each. Nothing is allocated. `format(...)` writes the same parts into a new `String`.
