# Binary messages

The `wire` module reads binary messages into a struct marked `@wire` and writes them back. Many TCP and UDP protocols send messages like this: a header of fixed fields, then text or lists with their length in front. The compiler works out the layout of each struct when it builds the program, so reading a message is one pass over its bytes, with no lookups at run time.

## A first message

The fields of a `@wire` struct go on the wire in the order they are written, one after another, with no padding between them. An integer takes as many bytes as its type: one for `u8`, two for `u16`, four for `u32`. The high byte comes first, the order network protocols use.

```iris run
import hex, wire

// The header that starts every message of a small protocol.
@wire
struct Header {
    u8 version
    u8 kind
    u16 flags
    u32 length
}

!void main() {
    h := try wire.decode(Header, "\x01\x02\x00\x05\x00\x00\x01\x00")
    print("version {h.version}, kind {h.kind}, flags {h.flags}, length {h.length}")

    reply := Header{version: 1, kind: 3, flags: 0, length: 12}
    scoped String bytes = try wire.encode(reply)
    print("{bytes.len} bytes: {hex.encode(bytes)}")
}
```

```output
version 1, kind 2, flags 5, length 256
8 bytes: 010300000000000c
```

`Header` takes 1 + 1 + 2 + 4 = 8 bytes on the wire. `wire.decode(T, bytes)` reads a `T` from the bytes of exactly one message and returns a `!T`. `wire.encode(v)` gives the bytes of a value as a new `String`, which the caller frees.

In memory a `@wire` struct is an ordinary struct. Its fields keep their usual alignment, and integers are in the machine's own byte order. Only the bytes on the wire follow the rules of this chapter.

## Reading from a connection

`wire.read(T, conn)` reads one message from a `net.Conn` and `wire.write(conn, v)` writes one. A server reads messages in a loop until the client closes the connection:

```iris run
import net, wire

@wire
struct Request {
    u32 id
    u16 a
    u16 b
}

@wire
struct Reply {
    u32 id
    u32 sum
}

void serve(net.Conn conn) {
    defer conn.close()
    for {
        req := wire.read(Request, conn) catch e {
            return
        }
        wire.write(conn, Reply{id: req.id, sum: u32(req.a) + u32(req.b)}) catch e {
            return
        }
    }
}

void acceptOne(net.Listener *ln) {
    conn := ln.accept() catch e {
        return
    }
    serve(conn)
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    spawn acceptOne(&ln)

    conn := try net.dial("127.0.0.1", ln.port)
    defer conn.close()
    for i in 1..4 {
        try wire.write(conn, Request{id: u32(i), a: u16(i * 100), b: 7})
        r := try wire.read(Reply, conn)
        print("reply {r.id}: {r.sum}")
    }
}
```

```output
reply 1: 107
reply 2: 207
reply 3: 307
```

TCP delivers a stream of bytes with no marks between messages. One read from the socket can bring half a message, or two and a bit. `wire.read` takes care of that. It reads until a whole message is there, takes exactly its bytes, and leaves whatever follows for the next read. It works with the connection's buffer, so `readLine`, `readExactly` and `buffered` can be mixed with it on the same connection.

When the other side closes before the first byte of a message, `wire.read` fails with `NetError.closed`. That is the normal end of a connection. Closing in the middle of a message is `WireError.short`. The connection's `setTimeout` limits each wait, as for any other read.

## Byte order

`@wire(le)` on the struct puts the low byte first in all its fields, the order of most file formats and of protocols that come from x86 machines. A field can choose its own order with `@wire(be)` or `@wire(le)`:

```iris run
import hex, wire

@wire(le)
struct Sample {
    u16 channel
    i32 value
    f32 gain
    @wire(be) u16 port
}

!void main() {
    s := Sample{channel: 1, value: -2, gain: 1.5, port: 8080}
    scoped String bytes = try wire.encode(s)
    print(hex.encode(bytes))

    back := try wire.decode(Sample, bytes)
    print("{back.channel} {back.value} {back.gain} {back.port}")
}
```

```output
0100feffffff0000c03f1f90
1 -2 1.5 8080
```

Floats are sent as their IEEE 754 bits, in the same byte order as integers. The types `u16be`, `u32be` and the rest aren't used in a `@wire` struct: write `u16`, and let the struct or `@wire(be)` say the order.

## What each type takes

| Type | On the wire |
|---|---|
| `u8`, `i8` | 1 byte |
| `u16`, `i16` | 2 bytes |
| `u32`, `i32`, `f32` | 4 bytes |
| `u64`, `i64`, `f64` | 8 bytes |
| `bool` | 1 byte, 0 or 1 |
| an enum with `@wire(u8)`, `@wire(u16)` or `@wire(u32)` | 1, 2 or 4 bytes |
| `T[n]` | n values of `T`, one after another |
| a `@wire` struct, as a field or embedded | its fields, in line |
| bitfields that share an integer | that integer |
| `String`, `T[]` | a length, then the bytes or items |

Structs inside structs add no bytes of their own. A field that is a `@wire` struct is its fields, in its own byte order. An array of four `u8` is four bytes, the usual way to hold an IPv4 address.

Bitfields work as they do in any struct: fields next to each other share one integer while their bits fit, and the first takes the highest bits, the order protocol diagrams use. On the wire that integer is written like any other.

```iris run
import wire

@wire
struct Ipv4Start {
    u8 version: 4
    u8 ihl: 4
    u8 tos
    u16 length
    u16 id
    u16 flags: 3
    u16 fragment: 13
    u8 ttl
    u8 protocol
}

!void main() {
    ip := try wire.decode(Ipv4Start, "\x45\x00\x00\x3c\x1c\x46\x40\x00\x40\x06")
    print("IPv{ip.version}, {ip.ihl * 4} byte header, {ip.length} bytes, flags {ip.flags}, ttl {ip.ttl}")
}
```

```output
IPv4, 20 byte header, 60 bytes, flags 2, ttl 64
```

`Duration`, pointers, `str` and plain enums have no fixed form on the wire, and the compiler says so when a `@wire` struct holds one.

## Enums

An enum in a `@wire` struct needs `@wire(u8)`, `@wire(u16)` or `@wire(u32)` on its declaration, the integer its values are sent as. The compiler checks that each value fits. Reading a number that isn't one of the values fails with `WireError.badValue`, so a decoded enum always holds one of its values. A flag enum takes any mix of its flags.

```iris run
import wire

@wire(u8)
enum Kind { hello = 1, data = 2, bye = 3 }

@wire(u8)
enum Options flags { compress, encrypt, ack }

@wire
struct Frame {
    Kind kind
    Options options
}

!void main() {
    f := try wire.decode(Frame, "\x02\x05")
    print("{f.kind}, compressed {f.options.has(Options.compress)}, encrypted {f.options.has(Options.encrypt)}")

    _ = wire.decode(Frame, "\x09\x00") catch e {
        print("{e.code}: {e.message}")
        f
    }
}
```

```output
data, compressed true, encrypted false
WireError.badValue: kind: 9 isn't one of the enum's values
```

A `bool` is the same: a byte other than 0 or 1 is `WireError.badValue`.

## Text and lists

A `String` or a slice has a length, and the bytes on the wire must say it. `@wire(len: u16)` puts the length in front, as a `u16`. `u8`, `u32` and `u64` work too. For a `String` the length counts bytes, for a slice it counts items.

```iris run
import hex, wire

@wire
struct Login {
    u8 version
    @wire(len: u8) String user
    @wire(len: u16) u16[] ports
}

!void main() {
    u16[] ports = alloc(u16, 2)
    ports[0] = 80
    ports[1] = 443
    scoped Login l = Login{version: 1, user: "ada".copy(), ports: ports}

    scoped String bytes = try wire.encode(l)
    print(hex.encode(bytes))

    scoped Login back = try wire.decode(Login, bytes)
    print("{back.user} wants {back.ports.len} ports, the last {back.ports[1]}")
}
```

```output
01036164610002005001bb
ada wants 2 ports, the last 443
```

Some protocols keep a list's length in an earlier field of the header, apart from the list. `@wire(len: count)` names that field. Reading takes the length from it. Encoding writes the slice's length into it, whatever the field holds, so the two can't disagree:

```iris run
import hex, wire

@wire
struct Name {
    u16 id
    @wire(len: u8) String text
}

@wire
struct Names {
    u8 count
    u8 flags
    @wire(len: count) Name[] items
}

!void main() {
    Name[] items = alloc(Name, 2)
    items[0] = Name{id: 1, text: "eth0".copy()}
    items[1] = Name{id: 2, text: "wg0".copy()}
    scoped Names n = Names{flags: 1, items: items}

    scoped String bytes = try wire.encode(n)
    print(hex.encode(bytes))

    scoped Names back = try wire.decode(Names, bytes)
    print("{back.count} names: {back.items[0].text} {back.items[1].text}")
}
```

```output
020100010465746830000203776730
2 names: eth0 wg0
```

A slice holds values of a fixed form or `@wire` structs. A slice of `String`s isn't allowed, since nothing would say each one's length. Put the `String` in a `@wire` struct, as `Name` does here, and use a slice of that.

## Limits

A length comes from the other side, so it can't be trusted. `@wire(max: n)` sets the most a field may hold, in bytes for a `String` and in items for a slice. A length above it fails with `WireError.tooLong` as soon as it is read, before any memory is taken for it.

```iris run
import wire

@wire
struct Upload {
    @wire(len: u32, max: 64KiB) String data
}

!void main() {
    _ = wire.decode(Upload, "\x10\x00\x00\x00") catch e {
        print("{e.code}: {e.message}")
        Upload{}
    }
}
```

```output
WireError.tooLong: data: 268435456 bytes, more than the max of 65536
```

A `u8` or `u16` length can't say more than 255 or 65535, which is a limit already. A length that can pass 65535 needs a `max`, or the program doesn't compile. Without one, four bytes from a stranger could make a server wait for and allocate 4 GiB.

`wire.encode` and `wire.write` check the same limits, and fail with `WireError.tooLong` instead of sending a message the other side would refuse.

## Memory

A decoded message owns its `String`s and slices. A `@wire` struct without a `free` method gets one that frees them, so `scoped` works on it. A struct with only fixed fields owns nothing, and needs no `free`.

When `wire.read` or `wire.decode` fails, it has already freed what it read.

A message that fits in the connection's buffer, 16 KiB, is read straight from it. A bigger one is gathered in memory of its own as it comes in, up to the limits its fields set.

## Errors

`wire.read`, `wire.decode`, `wire.encode` and `wire.write` fail with a `WireError`, or a `NetError` from the connection:

| Code | When |
|---|---|
| `short` | the bytes or the connection end in the middle of a message |
| `tooLong` | a `String` or slice is longer than its `max`, or than its length can say |
| `badValue` | a `bool` or enum holds a value it can't |
| `extra` | `wire.decode` was given bytes left over after the message |

The message names the field. After an error on a connection, the bytes that follow can't be trusted to start a new message, so a server closes the connection.

```iris run
import wire

@wire
struct Hello {
    u8 version
    bool resume
    @wire(len: u8) String name
}

void check(str bytes) {
    h := wire.decode(Hello, bytes) catch e match e.code {
        wire.WireError.short => {
            print("too short: {e.message}")
            return
        }
        _ => {
            print("{e.code}: {e.message}")
            return
        }
    }
    print("hello from {h.name}")
    h.name.free()
}

void main() {
    check("\x01\x00\x03ada")
    check("\x01\x00\x05ada")
    check("\x01\x02\x03ada")
    check("\x01\x00\x03ada!")
}
```

```output
hello from ada
too short: the bytes end in the middle of the message
WireError.badValue: resume: 2 isn't a bool, which is 0 or 1
WireError.extra: 1 byte is left after the message
```

## Datagrams and files

`wire.decode` reads from any bytes: a UDP datagram, a part of a file, or bytes another protocol carried. It wants the bytes of exactly one message, which is what a datagram is.

```iris run
import net, wire

@wire
struct Probe {
    u32 seq
    u64 sentAt
}

!void main() {
    server := try net.listenUdp(0, "127.0.0.1")
    client := try net.dialUdp(server.localAddr())
    for i in 1..3 {
        scoped String bytes = try wire.encode(Probe{seq: u32(i), sentAt: u64(1000 * i)})
        try client.send(bytes)
    }

    u8[1500] buf
    for _ in 1..3 {
        n, from := try server.readFrom(buf[0:])
        p := wire.decode(Probe, buf[0:n]) catch e {
            continue
        }
        print("probe {p.seq}, sent at {p.sentAt}, {n} bytes")
    }
}
```

```output
probe 1, sent at 1000, 12 bytes
probe 2, sent at 2000, 12 bytes
```

## How it works

For each `@wire` struct a program reads or writes, the compiler writes the code that reads it, sizes it and writes it, from its fields. A run of fixed fields is checked once for its bytes, then each field is a load, a byte swap when the order needs one, and a store. Nothing is looked up at run time, and a struct of fixed fields knows its size before the first byte arrives.

Mistakes in a `@wire` struct are compile errors: a type with no form on the wire, a `String` without a length, a length field that comes after its list, an enum value that doesn't fit its bytes, a length with no `max` that could ask for gigabytes.
