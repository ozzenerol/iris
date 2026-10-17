# Pointers and memory

Iris has no garbage collector. The program allocates memory and frees it. This page covers pointers, slices, allocation, and the tools that make freeing hard to forget.

## Pointers

`&` takes the address of a value and `*` reads or writes through a pointer:

```iris
i64 x = 5
i64 *p = &x
*p = 6 // x is now 6
```

A pointer to a struct reaches its fields with a dot:

```iris
Point *pp = &p
pp.x = 10
```

A pointer can be `null`. In a debug build, using a `null` pointer stops the program with an error that says where.

`u8 *` also stands for C's `void *`: any pointer converts to `u8 *` on its own, and back:

```iris
i64 x = 5
u8 *raw = &x
i64 *back = raw
```

Pointer arithmetic works as in C:

```iris
u8 *end = start + len
```

## Arrays

An array has a fixed size, written in the type:

```iris
u8[512] buf
i64[3] counts = [1, 2, 3]
```

## Slices

A slice, `T[]`, is a pointer to some items plus their count. It is the usual way to pass a list of things to a function:

```iris
i64 sum(i64[] xs) {
    i64 total
    for x in xs {
        total += x
    }
    return total
}

i64[3] counts = [1, 2, 3]
s := sum(counts) // an array is passed as a slice
```

`a[i:j]` takes items `i` to `j - 1`. Either end can be left out. A slice shares memory with what it was taken from:

```iris
n := try conn.read(buf)
handle(buf[0:n])
```

Every index is checked. Reading past the end stops the program with an error that says where.

`.ptr` gives a pointer to a slice's first item, for passing to C. A pointer can be sliced with both ends written, `p[0:n]`, to make a slice of memory that C handed over. That slice isn't checked against anything, since a pointer has no length.

A complete program:

```iris run
i64 sum(i64[] xs) {
    i64 total
    for x in xs {
        total += x
    }
    return total
}

void main() {
    i64[] items = alloc(i64, 4)
    for i in 0..4 {
        items[i] = i * 10
    }
    print("{sum(items)} {sum(items[2:])}")
    free(items)
}
```

```output
60 50
```

## Locals start at zero

Every local variable starts at zero, whether it is a number, a pointer, an array or a struct.

## Allocating and freeing

`alloc` returns a slice of new memory, set to zero. `free` gives it back.

```iris
u8[] buf = alloc(u8, 4KiB)
// use buf
free(buf)
```

`alloc` uses the default allocator. See [Allocators](#allocators) for others.

## defer

`defer` runs a statement when the block ends, however it ends: at the closing brace, at a `return`, or when `try` passes an error up. It keeps the cleanup next to the code that needs it:

```iris
!void copy(str from, to) {
    src := try fs.open(from)
    defer src.close()
    dst := try fs.create(to)
    defer dst.close()
    try io.copy(dst, src)
}
```

Several defers run in reverse order: `dst` is closed first, then `src`.

## errdefer

`errdefer` runs only when the function returns an error. It undoes work that should be kept only if everything succeeds:

```iris
!Server listen(u16 port) {
    fd := try net.socket()
    errdefer net.close(fd)
    try net.bind(fd, port)
    try net.listen(fd, 128)
    return Server{fd: fd}
}
```

If `bind` or `listen` fails, the socket is closed. If they succeed, it stays open.

A complete program:

```iris run
void work() {
    defer print("cleanup 1")
    defer print("cleanup 2")
    for i in 0..3 {
        defer print("end of round {i}")
        if i == 1 {
            break
        }
    }
    print("work done")
}

void main() {
    work()
}
```

```output
end of round 0
end of round 1
work done
cleanup 2
cleanup 1
```

## scoped

`scoped` on a declaration calls the value's `free` method at the end of the block:

```iris
scoped String body = try fs.readAll("index.html")
scoped Buffer b = buffer(4KiB)
```

It is the same as writing the declaration followed by `defer body.free()`. The type can come from the value, as with `:=`:

```iris
scoped t := term.table(["NAME", "SIZE"])
```

## Allocators

An allocator is a value that hands out memory. Functions in the standard library that allocate take an allocator as their last parameter, which defaults to the general one. Passing another allocator changes where the memory comes from.

An arena hands out memory from one big block and frees all of it at once. It suits work with a clear end, like handling one request:

```iris
void handle(Request *r) {
    scoped Arena a = arena(1MiB)
    page := render(r, &a)
    r.reply(page)
}
```

Everything `render` allocated from `a` is freed when `handle` returns.

## Checks in debug builds

A debug build stops the program with an error and a trace when it finds:

- an index past the end of an array or slice
- an integer that overflows
- a `null` pointer being used
- memory used after it was freed, or freed twice

The last check uses AddressSanitizer, which comes with `clang`. Its report is longer than a panic. It shows where the memory was used, freed and allocated, as Iris lines:

```output
ERROR: AddressSanitizer: heap-use-after-free
    #0 in f.main.use main.iris:2:13
    #1 in f.main.main main.iris:8:9
```

A release build keeps the index checks. A function marked `@unchecked` has none:

```iris
@unchecked
u32 checksum(u8[] data) {
}
```
