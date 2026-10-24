# Arrays and slices

An array holds a fixed number of values in a row. A slice views some of them, or memory from `alloc`, and knows its length. Most functions take slices.

## Arrays

The type is the item type followed by the size in brackets. A literal lists items in brackets.

```iris run
void main() {
    u16[4] ports = [22, 80, 443, 8080]
    str[2] resolvers = ["1.1.1.1", "8.8.8.8"]
    u8[16] buffer

    print("{ports[0]} {ports[3]} {resolvers[1]} {buffer[15]} {ports.len}")
}
```

```output
22 8080 8.8.8.8 0 4
```

- The size is part of the type and known at compile time, so `ports.len` is free.
- An array without a value is all zeros.
- A literal must have exactly as many items as the array.

Indexes run from 0 to `len - 1` and are checked. An out-of-range index stops the program with the location. A constant index is checked at compile time.

```output
panic: index 3 is out of range for length 3
  at main.iris:4:14 in main
```

Arrays are values. Assigning or passing one copies every item.

```iris run
void main() {
    u8[4] ip = [10, 0, 0, 1]
    next := ip
    next[3] = 2

    print("{ip[3]} {next[3]}")
}
```

```output
1 2
```

Nested array sizes read left to right: `i64[3][2]` is two arrays of three `i64`s, and the first index picks one of the two. Here two servers each count replies in three buckets (fast, slow, failed):

```iris run
void main() {
    i64[3][2] replies
    replies[1][2] = 5 // server 1, failed

    print("{replies.len} servers, {replies[0].len} buckets, {replies[1][2]} failed on server 1")
}
```

```output
2 servers, 3 buckets, 5 failed on server 1
```

## Slices

A slice `T[]` is a pointer and a count. It doesn't own its items. An array converts to a slice of its item type, so a function taking a slice accepts arrays of any size.

```iris run
i64 total(i64[] sizes) {
    i64 sum
    for s in sizes {
        sum += s
    }
    return sum
}

void main() {
    i64[3] reads = [4096, 4096, 812]
    i64[5] writes = [100, 200, 300, 400, 500]

    print("{total(reads)} {total(writes)} {total([1460, 40])}")
}
```

```output
9004 1500 1500
```

A slice shares items with its source, so writes through it change the original:

```iris run
void zero(u8[] buf) {
    for i in 0..buf.len {
        buf[i] = 0
    }
}

void main() {
    // the start of an IPv4 header; bytes 10 and 11 are the checksum
    u8[12] header = [0x45, 0, 0, 60, 0x1c, 0x46, 0x40, 0, 64, 6, 0xb1, 0xe6]

    zero(header[10:12]) // the checksum is computed with these at zero
    print("{header[9]} {header[10]} {header[11]}")
}
```

```output
6 0 0
```

## Slicing

`a[from:to]` is a slice of items `from` to `to - 1`. `a[:n]` is the first n items and `a[n:]` the rest. Slicing arrays, slices and `str` copies nothing.

```iris run
void main() {
    // a UDP header: source port, destination port, length, checksum, 2 bytes each
    u8[8] udp = [0x1f, 0x90, 0x00, 0x35, 0x00, 0x1c, 0x00, 0x00]

    src := udp[0:2]
    print("{src.len} bytes: source port {u16(src[0]) << 8 | u16(src[1])}")

    ports := udp[:4]
    rest := udp[4:]
    print("{ports.len} {rest.len}, length {rest[1]}")

    dst := ports[2:]
    print("destination port {dst[1]}")
}
```

```output
2 bytes: source port 8080
4 4, length 28
destination port 53
```

Both ends are checked: the end can't pass the source's end, and the start can't pass the end. `a[n:n]` is empty.

`[]` is an empty slice of any type:

```iris run
// The protocols a TLS client offers (ALPN).
void offer(str[] protocols) {
    print("{protocols.len} protocols offered")
}

void main() {
    offer([])
    offer(["h2", "http/1.1"])
}
```

```output
0 protocols offered
2 protocols offered
```

## Allocated slices

`alloc(T, n)` returns a slice of `n` zeroed items. `free` releases it.

```iris run
void main() {
    clients := 5 // known only at run time
    i64[] sent = alloc(i64, clients)
    defer free(sent)

    for i in 0..clients {
        sent[i] = 1460 * (i + 1)
    }

    print("client 4 sent {sent[4]} bytes; {sent.len} clients")
}
```

```output
client 4 sent 7300 bytes; 5 clients
```

`defer` frees it on every exit from the block (see [Memory](19-memory.html)). An allocated slice has a fixed length. For a growing list, use `List` (see [Collections](22-collections.html)).

## Slice fields

`.len` is the item count and `.ptr` the pointer to the first item, for C functions. A pointer sliced with both ends, `p[0:n]`, views C memory as a slice. That slice is unchecked, since a pointer has no length.

```iris run
void main() {
    u8[4] ip = [127, 0, 0, 1]
    s := ip[0:4]

    u8 *p = s.ptr // what a C function such as inet_ntop takes
    again := p[0:2]

    print("{s.len} {again[0]}")
}
```

```output
4 127
```

## Text is a slice

A `str` is a `u8[]`. Indexing gives a byte, slicing gives a `str`, and `for` iterates bytes (see [Text](06-text.html)).
