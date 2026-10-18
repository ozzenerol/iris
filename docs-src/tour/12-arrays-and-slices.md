# Arrays and slices

An array is a fixed number of values stored one after another. A slice looks at some of them, or at memory from `alloc`, and knows how many there are. Slices are how most functions take a list of things.

## Arrays

An array's type is its item type followed by its size in brackets. A literal lists the items in brackets too:

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

The size is part of the type and is known when compiling, so `ports.len` costs nothing. An array declared without a value has every item at zero. A literal must have as many items as the array.

An index runs from 0 to `len - 1`. Every index is checked: one outside the array stops the program with a message that says where. When the index is a constant, the compiler checks it before the program runs:

```output
panic: index 3 is out of range for length 3
  at main.iris:4:14 in main
```

Arrays are values. Assigning one copies all its items, and so does passing it to a function that takes an array:

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

An array of arrays is written with the sizes one after the other, read from the left: `i64[3][2]` is two arrays of three `i64`s each. The first index picks one of the two. Here each of two servers counts its replies in three buckets: fast, slow, failed:

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

A slice, `T[]`, is a pointer to items and their count. It doesn't own them: they live in an array, in memory from `alloc`, or anywhere else. An array can be given wherever a slice of its item type is wanted, so a function that takes a slice works with an array of any size:

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

A slice looks at the same items as what it came from, so changing an item through it changes the original:

```iris run
void zero(u8[] buf) {
    for i in 0..buf.len {
        buf[i] = 0
    }
}

void main() {
    // an IPv4 header starts with these; bytes 10 and 11 are its checksum
    u8[12] header = [0x45, 0, 0, 60, 0x1c, 0x46, 0x40, 0, 64, 6, 0xb1, 0xe6]
    zero(header[10:12]) // the checksum is worked out with these at zero
    print("{header[9]} {header[10]} {header[11]}")
}
```

```output
6 0 0
```

## Slicing

`a[from:to]` takes the items from `from` up to `to - 1`, as a slice. Either end can be left out: `a[:n]` is the first n items and `a[n:]` all but the first n. Slicing an array, a slice or a `str` copies nothing:

```iris run
void main() {
    // a UDP header: source port, destination port, length, checksum, two bytes each
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

Both ends are checked: a slice can't reach past the end of what it is taken from, and its start can't be after its end. `a[n:n]` is an empty slice.

`[]` is an empty slice of any type, for a parameter with nothing to give:

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

## Slices of new memory

`alloc(T, n)` gives a slice of `n` new items of type `T`, all at zero. `free` gives the memory back. Between the two, the slice is used like any other:

```iris run
void main() {
    clients := 5 // known only when the program runs
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

`defer` frees it when the block ends, on every way out (see [Memory](19-memory.html)). A slice from `alloc` has a fixed length; a list that grows is a `List` (see [Collections](22-collections.html)).

## A slice's parts

`.len` is a slice's count of items and `.ptr` the pointer to its first one, for C functions. A pointer can be sliced with both ends given, `p[0:n]`, to look at memory that came from C as a slice. That slice isn't checked against anything, since a pointer has no length:

```iris run
void main() {
    u8[4] ip = [127, 0, 0, 1]
    s := ip[0:4]
    u8 *p = s.ptr // what a C function such as inet_ntop would take
    again := p[0:2]
    print("{s.len} {again[0]}")
}
```

```output
4 127
```

## Text is a slice

A `str` is a slice of bytes, `u8[]`, so everything here works on text too: indexing gives a byte, slicing gives a `str`, and `for` goes over its bytes (see [Text](06-text.html)).
