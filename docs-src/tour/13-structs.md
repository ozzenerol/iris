# Structs

A struct puts several values together into one type, each with a name. This chapter covers declaring structs, making values of them, defaults, copying and comparing, and embedding one struct in another.

## Declaring a struct

A struct lists its fields, one on each line, each with its type and name:

```iris run
struct Point {
    i64 x
    i64 y
}

void main() {
    p := Point{x: 3, y: 4}
    print("{p.x}, {p.y}")
    p.x = 10
    print("{p.x}, {p.y}")
}
```

```output
3, 4
10, 4
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
struct Point {
    i64 x
    i64 y
}

void draw(Point p) => print("drawing at {p.x}, {p.y}")

void main() {
    Point start = .{x: 1, y: 2}
    draw(.{x: 5, y: 6})
    draw(start)
}
```

```output
drawing at 5, 6
drawing at 1, 2
```

## Copying and comparing

A struct is a value: assigning it, or passing it to a function, copies every field. The copy and the original are separate:

```iris run
struct Point {
    i64 x
    i64 y
}

void main() {
    a := Point{x: 1, y: 2}
    b := a
    b.x = 100
    print("{a.x} {b.x}")
}
```

```output
1 100
```

`==` and `!=` compare two structs field by field, when every field can be compared that way: numbers, `bool`, `str`, enums, pointers and structs of those. A `String`, a slice or a `List` holds a pointer to memory it owns, so a struct with one can't be compared with `==`; the program compares the fields it means:

```iris run
struct Point {
    i64 x
    i64 y
}

struct Tag {
    str name
    i64 weight
}

void main() {
    print("{Point{x: 1, y: 2} == Point{x: 1, y: 2}} {Point{x: 1} == Point{x: 2}}")
    a := Tag{name: "new", weight: 1}
    b := Tag{name: "new", weight: 1}
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
struct Point {
    i64 x
    i64 y
}

struct Rect {
    Point min
    Point max
}

i64 area(Rect r) => (r.max.x - r.min.x) * (r.max.y - r.min.y)

void main() {
    r := Rect{min: Point{x: 1, y: 1}, max: Point{x: 4, y: 3}}
    r.max.x = 6
    print("area {area(r)}")
}
```

```output
area 10
```

A struct can't hold itself, since its size would have no end. It can hold a pointer to itself, or a `List` of itself, whose items are kept apart:

```iris run
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
    root := Node{name: "root"}
    defer root.free()
    leaf := Node{name: "leaf"}
    branch := Node{name: "branch"}
    branch.children.push(leaf)
    root.children.push(branch)
    root.children.push(Node{name: "other"})
    print("{root.count()} nodes")
}
```

```output
4 nodes
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
