# Structs

Grouping values into one type, and adding methods to it.

## Declaring a struct

Fields are written one per line, each with its type:

```iris
struct Point {
    i64 x
    i64 y
}
```

A field can have a default value. Fields without one start at zero.

```iris
struct Config {
    str host = "localhost"
    u16 port = 8080
    bool verbose
}
```

## Making a value

A struct literal names the fields it sets. Fields left out get their default:

```iris
p := Point{x: 1, y: 2}
c := Config{port: 9000} // host is "localhost", verbose is false
```

When the type is already known, the literal can start with a dot instead of the name:

```iris
Point p = .{x: 1, y: 2}
draw(.{x: 5, y: 5})
```

Fields are read and changed with a dot:

```iris
p.x = 10
print("{p.x}, {p.y}")
```

## Values are copied

Assigning a struct copies all its fields. The two copies are separate:

```iris
a := Point{x: 1, y: 2}
b := a
b.x = 100 // a.x is still 1
```

`==` compares two structs field by field.

## Methods

A method is a function declared with the type's name in front. Inside it, `self` is a pointer to the value it was called on:

```iris
i64 Point.dist2(Point other) {
    dx := self.x - other.x
    dy := self.y - other.y
    return dx * dx + dy * dy
}

void Point.move(i64 dx, dy) {
    self.x += dx
    self.y += dy
}
```

Methods are called with a dot:

```iris
p := Point{x: 1, y: 2}
p.move(3, 3)
d := p.dist2(Point{})
```

Because `self` is a pointer, a method can change the value. When a method is called on a variable, the method gets the variable's address. When it is called on a temporary value, like `origin().dist2(p)`, the method gets the address of a copy. A method can be declared in any file of the module that declares the type.

A complete program:

```iris run
struct Point {
    i64 x
    i64 y
}

void Point.move(i64 dx, dy) {
    self.x += dx
    self.y += dy
}

i64 Point.dist2(Point other) {
    dx := self.x - other.x
    dy := self.y - other.y
    return dx * dx + dy * dy
}

void main() {
    p := Point{x: 1, y: 2}
    p.move(2, 2)
    print("{p.x}, {p.y}")
    print("{p.dist2(Point{})}")
}
```

```output
3, 4
25
```

## Making and freeing

By convention, a function named after the type in lower case makes a value, and a `free` method releases what it holds:

```iris
struct Buffer {
    u8[] data
    i64 used
}

Buffer buffer(i64 size) => Buffer{data: alloc(u8, size)}

void Buffer.free() {
    free(self.data)
}
```

`scoped` calls `free` at the end of the block:

```iris
scoped Buffer b = buffer(4KiB)
```

## Embedding

A struct can include another struct by writing its type without a field name. The embedded struct's fields and methods can be used as if they belonged to the outer struct:

```iris
struct Socket {
    i32 fd
}

void Socket.write(u8[] data) { }

struct Conn {
    Socket
    str peer
}

c.write(data)   // calls Socket.write
print("{c.fd}") // the Socket's fd
```

Iris has no inheritance. Embedding is how one type reuses another.
