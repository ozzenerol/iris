# Pointers

A pointer holds the address of a value, so code can read and change the value where it is instead of a copy of it. This chapter covers taking addresses, reading and writing through pointers, `null`, and the rules that keep pointers valid.

## Taking an address

`&` gives the address of a variable, a field or an item. The pointer's type is the value's type with a `*` after it. `*p` is the value the pointer points to, for reading and for writing:

```iris run
void main() {
    i64 open = 5
    i64 *p = &open
    print("{*p}")
    *p = 6
    print("{open}")
    *p += 10
    print("{open}")
}
```

```output
5
6
16
```

A pointer can point to a field or an item, as well as to a whole variable:

```iris run
struct Stats {
    i64 sent
    i64 dropped
}

void main() {
    s := Stats{sent: 100, dropped: 2}
    i64 *d = &s.dropped
    *d += 1
    i64[3] perCore = [10, 20, 30]
    i64 *last = &perCore[2]
    *last += 5
    print("{s.dropped} {perCore[2]}")
}
```

```output
3 35
```

## Pointers to structs

A pointer to a struct reaches the struct's fields and methods with a dot, as the struct itself does: there is no separate operator for it.

```iris run
struct Conn {
    str peer
    i64 sent
}

void record(Conn *c, i64 bytes) {
    c.sent += bytes
}

void main() {
    conn := Conn{peer: "10.0.0.9:51000", sent: 100}
    record(&conn, 1460)
    Conn *p = &conn
    p.sent += 40
    print("{conn.peer}: {conn.sent} bytes")
}
```

```output
10.0.0.9:51000: 1600 bytes
```

This is the usual reason for a pointer parameter: the function changes the caller's value. Methods get `self` as a pointer for the same reason.

## null

A pointer can be `null`: it points to nothing. A pointer variable declared without a value starts as `null`, and `==` and `!=` compare pointers with each other and with `null`:

```iris run
// A message kept as a chain of buffers, each pointing to the next.
struct Chunk {
    i64 size
    Chunk *next
}

i64 total(Chunk *c) {
    i64 sum = 0
    for c != null {
        sum += c.size
        c = c.next
    }
    return sum
}

void main() {
    tail := Chunk{size: 300}
    mid := Chunk{size: 4096, next: &tail}
    head := Chunk{size: 4096, next: &mid}
    print("{total(&head)} {total(null)}")
}
```

```output
8492 0
```

Reading or writing through a `null` pointer is a bug. A debug build stops the program with a message that says where:

```output
panic: null pointer used
  at main.iris:6:15 in main
```

## Pointers stay valid only while their value lives

A pointer to a variable is valid while the variable exists: until the end of its block for a local, for the whole program for a global. A pointer into memory from `alloc` is valid until it is freed. Using a pointer after that is a bug that a debug build finds and reports (see [Memory](19-memory.html)).

The compiler stops the plain case, a function giving back a pointer or a slice into its own variables:

```iris error
i64 *counter() {
    i64 n = 0
    return &n // error: this points into n, which is gone when counter returns; return a copy, or memory from alloc
}
```

A function that needs to give back something that outlives it returns the value itself, or memory from `alloc` that the caller frees.

## Pointers and memory

`u8 *` is a pointer to bytes, and stands for "a pointer to anything", as C's `void *` does: any pointer converts to `u8 *` on its own, and a `u8 *` converts back to any pointer type. This is how memory is passed to and from C code:

```iris run
void main() {
    i32 fd = 3
    u8 *any = &fd // what a C function such as setsockopt takes
    i32 *back = any
    print("{*back}")
}
```

```output
3
```

Adding a number to a pointer moves it by that many items, not bytes, as in C:

```iris run
void main() {
    u16[4] ports = [22, 80, 443, 8080]
    u16 *p = &ports[0]
    u16 *third = p + 2
    print("{*third}")
}
```

```output
443
```

A pointer has no length, so the program can't check that `p + 2` is still inside the array. A slice knows its length and is checked on every index, which is why slices are the usual way to pass a run of items. A pointer can be turned into a slice when the length is known, with `p[0:n]`, for memory that came from C:

```iris run
void main() {
    u8[6] mac = [0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e]
    u8 *p = &mac[3] // the last three bytes name the device
    device := p[0:3]
    print("{device.len} {device[0]} {device[2]}")
}
```

```output
3 60 94
```

A pointer to a pointer is written with two stars, `T **`, and is mostly found in C functions that hand back a pointer through an argument.
