# Structs

A struct puts several values together into one type, each with a name. This chapter covers declaring structs, making values of them, defaults, copying and comparing, and embedding one struct in another.

## Declaring a struct

A struct lists its fields, one on each line, each with its type and name:

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

Fields are read and changed with a dot. A field can be of any type: a number, text, an array, another struct, a pointer, a function, a `List`.

## Making a value

A struct literal is the type's name and, in braces, the fields to set, by name. Fields left out are zero, or their default:

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

A default is a value known when compiling. Defaults apply wherever a value is made without one: a field left out of a literal, a variable or a global declared with no value (`Config c`), the items of an array of structs, and a struct inside another struct. Memory from `alloc` is the exception: it is all zeros, as it comes.

When the type is already known from where the value goes, the literal can start with a dot instead of the type's name:

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

A struct is a value: assigning it, or passing it to a function, copies every field. The copy and the original are separate:

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

`==` and `!=` compare two structs field by field, when every field can be compared that way: numbers, `bool`, `str`, enums, pointers and structs of those. A `String`, a slice or a `List` holds a pointer to memory it owns, so a struct with one can't be compared with `==`; the program compares the fields it means:

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

## Structs inside structs

A field can be another struct. Its fields are reached with one more dot:

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
    eth := Iface{name: "eth0", rx: Counters{packets: 10, bytes: 9000}, tx: Counters{packets: 4, bytes: 600}}
    eth.tx.bytes += 1500
    print("{eth.name}: {traffic(eth)} bytes")
}
```

```output
eth0: 11100 bytes
```

A struct can't hold itself, since its size would have no end. It can hold a pointer to itself, or a `List` of itself, whose items are kept apart:

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

A field written as just a type, without a name, embeds that struct. Its fields and methods can then be used as if they were the outer struct's own:

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

The embedded struct is a field named after its type, so it can also be reached and set as a whole: `c.Socket`. A method called through the outer struct gets a pointer to the embedded one as its `self`.

Embedding is how one type reuses another; there is no inheritance. When the outer struct has a field or method of the same name, its own one is used.

## Visible to other modules

A struct marked `pub` can be used by other modules, and so can those of its fields that are marked `pub`. The rest of its fields are the module's own: other modules can't read or set them, which keeps them free to change (see [Modules](23-modules.html)):

```iris
pub struct Server {
    pub u16 port // other modules can use it
    i32 fd       // only this module can
}
```
