# Pointers

A pointer holds the address of a value, so code can read and change the value in place. This chapter covers addresses, dereferencing, `null`, and how long a pointer stays valid.

## Addresses

`&` takes the address of a variable, field or item. The pointer type is the value's type followed by `*`. `*p` reads or writes the value.

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

Fields and array items have addresses too:

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

A struct pointer reaches fields and methods with a dot, the same as the struct:

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

This is the usual reason for a pointer parameter: the function changes the caller's value. `self` is a pointer for the same reason.

## null

`null` points to nothing. An unset pointer variable is `null`. `==` and `!=` compare pointers with each other and with `null`.

```iris run
// A message stored as a chain of buffers.
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

Using a `null` pointer is a bug. A debug build stops with the location:

```output
panic: null pointer used
  at main.iris:6:15 in main
```

## Lifetime

A pointer to a local is valid until the end of its block. A pointer to a global is valid for the whole program. A pointer into `alloc` memory is valid until it is freed. A debug build reports use after that (see [Memory](19-memory.html)).

The compiler rejects returning a pointer or slice into a function's own locals:

```iris error
i64 *counter() {
    i64 n = 0
    return &n // error: this points into n, which is gone when counter returns; return a copy, or memory from alloc
}
```

An array literal given back as a slice is rejected the same way, as it lives in the function too:

```iris error
i64[] primes() => [2, 3, 5] // error: this array is gone when primes returns; make it a global, or return memory from alloc
```

Return the value itself, a fixed array (`i64[3]`), a global, or `alloc` memory the caller frees.

## Raw memory

`u8 *` is the generic pointer, like C's `void *`. Every pointer converts to `u8 *` and back implicitly. C memory passes through it.

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

Adding n to a pointer moves it by n items:

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

A pointer has no length, so `p + 2` is unchecked. Slices know their length and check every index, which is why they are the usual way to pass items. `p[0:n]` turns a pointer into a slice when the length is known, for memory from C:

```iris run
void main() {
    u8[6] mac = [0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e]
    u8 *p = &mac[3] // the last three bytes identify the device
    device := p[0:3]

    print("{device.len} {device[0]} {device[2]}")
}
```

```output
3 60 94
```

`u64(p)` or `i64(p)` gives the address as a number, to check alignment or pass it to C:

```iris run
struct Slot align(64) {
    i64 hits
}

void main() {
    Slot[2] slots
    u64 a = u64(&slots[0])
    u64 b = u64(&slots[1])

    print("{b - a} apart, {a % 64} past a 64-byte line")
}
```

```output
64 apart, 0 past a 64-byte line
```

`T **` is a pointer to a pointer, mostly for C functions that return a pointer through an argument.
