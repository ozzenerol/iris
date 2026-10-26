## Reading and writing messages

A struct marked `@wire` describes a binary message. Its fields go on the wire in order, with no padding, the high byte first unless the struct says `@wire(le)`. A `String` or slice needs its length, from a prefix (`@wire(len: u16)`) or from an earlier field (`@wire(len: count)`), and a `max` when that length can pass 65535. An enum needs `@wire(u8)`, `@wire(u16)` or `@wire(u32)`. The [Binary messages chapter](../library-tour/21-binary-messages.html) of the Library Tour has every rule. This is the short version:

```iris run
import hex, wire

@wire(u8)
enum Kind { hello = 1, data = 2 }

@wire
struct Message {
    Kind kind
    u16 seq
    @wire(len: u16, max: 4KiB) String body
}

!void main() {
    scoped Message m = Message{kind: .data, seq: 7, body: "ping".copy()}
    scoped String bytes = try wire.encode(m)
    print(hex.encode(bytes))

    scoped Message back = try wire.decode(Message, bytes)
    print("{back.kind} {back.seq} {back.body}")
}
```

```output
020007000470696e67
data 7 ping
```

`wire.read(T, conn)` reads one message from a `net.Conn`, however the bytes arrive, and `wire.write(conn, v)` writes one.
