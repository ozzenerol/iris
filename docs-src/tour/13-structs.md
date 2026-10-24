# Structs

A struct groups named values into one type. This chapter covers declaring structs, literals and defaults, copying and comparing, embedding, and memory layout.

## Declaring

A struct lists its fields, one per line, each with a type and a name:

```iris run
struct Endpoint {
    str host
    u16 port
}

void main() {
    e := Endpoint{host: "10.0.0.5", port: 22}
    print("{e.host}:{e.port}")

    e.port = 2222
    print("{e.host}:{e.port}")
}
```

```output
10.0.0.5:22
10.0.0.5:2222
```

Fields are read and set with a dot. A field can have any type: number, text, array, struct, pointer, function, `List`.

## Literals and defaults

A literal is the type name followed by named fields in braces. Omitted fields take their default, or zero.

```iris run
struct Config {
    str host = "localhost"
    u16 port = 8080
    bool verbose
    Duration timeout = 30s
}

void main() {
    a := Config{}
    b := Config{port: 9000, verbose: true}

    print("{a.host}:{a.port} {a.verbose} {a.timeout}")
    print("{b.host}:{b.port} {b.verbose} {b.timeout}")
}
```

```output
localhost:8080 false 30s
localhost:9000 true 30s
```

A default must be known at compile time. Defaults apply wherever a value is made without one: omitted literal fields, variables and globals declared without a value (`Config c`), array items, and nested structs. Memory from `alloc` is the exception and is all zeros.

A `String` field can default to text. The field borrows the text, so creating the value allocates nothing and freeing the field frees nothing. The first change copies the text, and after that it is an ordinary `String`.

```iris run
struct Server {
    String name = "edge"
    u16 port = 443
}

void main() {
    s := Server{}
    t := Server{port: 8443}
    t.name.append("-2")

    print("{s.name}:{s.port} {t.name}:{t.port}")

    s.name.free()
    t.name.free()
}
```

```output
edge:443 edge-2:8443
```

This suits `@json` structs, whose fields keep their defaults when the file omits them (see [Configuration files](../library-tour/08-configuration.html)).

When the type is known from context, the literal can start with a dot:

```iris run
struct Endpoint {
    str host
    u16 port
}

void connect(Endpoint e) => print("connecting to {e.host}:{e.port}")

void main() {
    Endpoint db = .{host: "db.lan", port: 5432}

    connect(.{host: "cache.lan", port: 6379})
    connect(db)
}
```

```output
connecting to cache.lan:6379
connecting to db.lan:5432
```

## Copying and comparing

Structs are values. Assigning or passing one copies every field.

```iris run
struct Endpoint {
    str host
    u16 port
}

void main() {
    plain := Endpoint{host: "example.com", port: 80}
    secure := plain
    secure.port = 443

    print("{plain.port} {secure.port}")
}
```

```output
80 443
```

`==` and `!=` compare field by field when every field is comparable: numbers, `bool`, `str`, enums, pointers, and structs of those. A struct with a `String`, slice or `List` field can't use `==`. Compare the fields you mean instead.

```iris run
struct Addr {
    u32 ip
    u16 port
}

struct Route {
    str path
    i64 weight
}

void main() {
    print("{Addr{ip: 1, port: 80} == Addr{ip: 1, port: 80}} {Addr{port: 80} == Addr{port: 443}}")

    a := Route{path: "/api", weight: 1}
    b := Route{path: "/api", weight: 1}
    print("{a == b}")
}
```

```output
true false
true
```

## Nested structs

A field can be a struct, reached with another dot:

```iris run
struct Counters {
    i64 packets
    i64 bytes
}

struct Iface {
    str name
    Counters rx
    Counters tx
}

i64 traffic(Iface i) => i.rx.bytes + i.tx.bytes

void main() {
    eth := Iface{
        name: "eth0",
        rx: Counters{packets: 10, bytes: 9000},
        tx: Counters{packets: 4, bytes: 600},
    }

    eth.tx.bytes += 1500
    print("{eth.name}: {traffic(eth)} bytes")
}
```

```output
eth0: 11100 bytes
```

A struct can't contain itself, but it can hold a pointer to itself or a `List` of itself:

```iris run
// One part of a URL path, with the parts that can follow it.
struct Node {
    str name
    List[Node] children
}

void Node.free() {
    for c in self.children {
        c.free()
    }
    self.children.free()
}

i64 Node.count() {
    i64 n = 1
    for c in self.children {
        n += c.count()
    }
    return n
}

void main() {
    root := Node{name: "/"}
    defer root.free()

    users := Node{name: "users"}
    api := Node{name: "api"}
    api.children.push(users)

    root.children.push(api)
    root.children.push(Node{name: "static"})

    print("{root.count()} paths")
}
```

```output
4 paths
```

## Embedding

A field written as a type without a name embeds that struct. Its fields and methods work directly on the outer struct.

```iris run
struct Socket {
    i32 fd
}

void Socket.send(str data) => print("fd {self.fd} sends {data}")

struct Conn {
    Socket
    str peer
}

void main() {
    c := Conn{Socket: Socket{fd: 3}, peer: "10.0.0.2"}

    c.send("hello")
    print("{c.fd} {c.Socket.fd} {c.peer}")
}
```

```output
fd 3 sends hello
3 3 10.0.0.2
```

- The embedded struct is a field named after its type, so `c.Socket` reaches it as a whole.
- A method called through the outer struct gets a pointer to the embedded struct as `self`.
- There is no inheritance. If the outer struct has a field or method with the same name, its own wins.

## Memory layout

Fields are stored in declaration order. Each starts at a multiple of its size, so padding can appear between fields, and the struct can be passed to C unchanged. `sizeof(T)` gives the size including padding, and works on array types such as `sizeof(Entry[4])`.

```iris run
struct Entry {
    u8 kind
    u32 addr
    u16 port
}

void main() {
    print("{sizeof(Entry)} {sizeof(u32)}")
}
```

```output
12 4
```

Three features match byte-exact formats such as protocol headers:

- `packed` removes padding. Each field starts right after the previous one.
- `u16be`, `u32be`, `u64be`, `i16be`, `i32be` and `i64be` store the high byte first, as network protocols do. They read and write as plain integers, and the bytes are swapped on access.
- `name: n` makes a bitfield of `n` bits. Adjacent bitfields share one integer of their type while the bits fit, and the first takes the highest bits, the order protocol diagrams use.

`u8 *` converts to any pointer, so a header can be read straight from received bytes:

```iris run
struct Ipv4 packed {
    u8 version: 4
    u8 ihl: 4
    u8 tos
    u16be length
    u16be id
    u16be flags: 3
    u16be fragment: 13
    u8 ttl
    u8 protocol
    u16be checksum
    u8[4] src
    u8[4] dst
}

void main() {
    u8[20] packet = [0x45, 0, 0, 60, 0x1c, 0x46, 0x40, 0, 64, 6, 0xb1, 0xe6, 192, 168, 0, 1, 10, 0, 0, 7]
    Ipv4 *ip = packet[0:20].ptr

    print("IPv{ip.version}, {ip.ihl * 4} byte header, {ip.length} bytes, ttl {ip.ttl}, protocol {ip.protocol}")
    print("from {ip.src[0]}.{ip.src[1]}.{ip.src[2]}.{ip.src[3]}, flags {ip.flags}")

    ip.ttl--
    ip.length = 1500
    print("{sizeof(Ipv4)} bytes, now {packet[2]} {packet[3]} {packet[8]}")
}
```

```output
IPv4, 20 byte header, 60 bytes, ttl 64, protocol 6
from 192.168.0.1, flags 2
20 bytes, now 5 220 63
```

Bitfields, byte-swapped fields and unaligned packed fields have no address, so `&ip.length` doesn't compile. Copy the value out first. A compile-time value must fit a bitfield. A debug build checks other values at run time.

```iris run
struct Flags {
    u8 syn: 1
    u8 ack: 1
    u8 window: 6 = 32
}

void main() {
    Flags f = .{syn: 1}
    f.ack = 1
    f.window += 31

    print("{f.syn} {f.ack} {f.window} {sizeof(Flags)}")
}
```

```output
1 1 63 1
```

`align(n)` starts the struct at a multiple of `n` bytes and rounds its size up to a multiple of `n`. Use it to keep per-thread counters on separate 64-byte cache lines so threads don't slow each other down. Variables, globals and `alloc` all respect it.

```iris run
struct Counter align(64) {
    i64 hits
}

void main() {
    Counter[4] perThread
    perThread[1].hits += 1

    print("{sizeof(Counter)} {sizeof(Counter) * 4}")
}
```

```output
64 256
```

## Visibility

A `pub` struct is visible to other modules, and so are its `pub` fields. Other fields are private to the module (see [Modules](23-modules.html)).

```iris
pub struct Server {
    pub u16 port // other modules can use it
    i32 fd       // only this module can
}
```
