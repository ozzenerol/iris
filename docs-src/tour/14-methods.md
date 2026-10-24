# Methods

A method is a function that belongs to a struct. This chapter covers `self`, constructor and `free` conventions, and the methods the language itself calls.

## Declaring

Write the struct name and a dot before the method name. `self` is a pointer to the receiver.

```iris run
struct Server {
    str name
    i64 open
}

void Server.accepted(i64 n) {
    self.open += n
}

str Server.load() => if self.open > 10 { "busy" } else { "quiet" }

void main() {
    s := Server{name: "edge-1"}

    s.accepted(4)
    s.accepted(9)

    print("{s.name}: {s.open} open, {s.load()}")
}
```

```output
edge-1: 13 open, busy
```

`self` is a pointer, so a method can change a variable it is called on. Called on a temporary, such as a function result, it works on a copy.

```iris run
// A TCP sender's window: the bytes it may send before an acknowledgement.
struct Window {
    i64 start // first unacknowledged byte
    i64 size
}

Window initial() => Window{size: 1460}

// In slow start the window grows by what was acknowledged.
void Window.ack(i64 bytes) {
    self.start += bytes
    self.size += bytes
}

i64 Window.end() => self.start + self.size

void main() {
    w := Window{start: 1000, size: 2920}
    w.ack(1460)

    print("may send {w.start} to {w.end()}")
    print("{initial().end()}")
}
```

```output
may send 2460 to 6840
1460
```

Methods are called the same way through a pointer: `wp.ack(1460)` changes what `wp` points to.

Methods can be declared in any file of the struct's module, in any order. A method can't share a name with a field or another method.

## Constructors and free

By convention, a function named after the type in lower case creates a value, and a `free` method releases what it holds. `scoped` calls `free` at the end of the block.

```iris run
struct Buffer {
    u8[] data
    i64 used
}

Buffer buffer(i64 size) => Buffer{data: alloc(u8, size)}

void Buffer.free() {
    free(self.data)
}

void Buffer.add(str text) {
    for b in text {
        self.data[self.used] = b
        self.used += 1
    }
}

str Buffer.text() => self.data[0:self.used]

void main() {
    scoped Buffer b = buffer(64)

    b.add("HTTP/1.1 ")
    b.add("200 OK")

    print("[{b.text()}] {b.used} of {b.data.len}")
}
```

```output
[HTTP/1.1 200 OK] 15 of 64
```

A method that can fail returns `!T`. A constructor that can fail returns `!Type`.

## Methods the language calls

| Method | Used by |
|---|---|
| `void free()` | `scoped`, and `List.freeEach` on each item |
| `String format()` | interpolation with braces |
| `(T, bool) next(i64 *at)` | `for` loops |
| `i64 len()` | `x.len`, like `List`, `String` and slices |

```iris run
struct PortRange {
    u16 first
    u16 last
}

String PortRange.format() => format("{self.first}-{self.last}")

(u16, bool) PortRange.next(i64 *at) {
    p := i64(self.first) + *at
    if p > i64(self.last) {
        return (0, false)
    }

    *at += 1
    return (u16(p), true)
}

void main() {
    r := PortRange{first: 8080, last: 8082}
    print("scanning {r}")

    for port in r {
        print("{port}")
    }
}
```

```output
scanning 8080-8082
8080
8081
8082
```

With a `len()` method and no `len` field, `.len` works like it does on every collection:

```iris run
struct Ring {
    i64[8] items
    i64 start
    i64 count
}

i64 Ring.len() => self.count

void main() {
    r := Ring{count: 3}
    p := &r

    print("{r.len} {p.len} {r.len()}")
}
```

```output
3 3 3
```

A struct with the methods an interface lists can be used as that interface (see [Interfaces](21-interfaces.html)).

## Methods through embedding

An embedded struct's methods can be called on the outer struct. They get the embedded struct as `self`.

```iris run
struct Logger {
    str prefix
}

void Logger.log(str msg) => print("[{self.prefix}] {msg}")

struct Service {
    Logger
    str name
}

void Service.start() {
    self.log("starting {self.name}")
}

void main() {
    s := Service{Logger: Logger{prefix: "svc"}, name: "api"}

    s.start()
    s.log("ready")
}
```

```output
[svc] starting api
[svc] ready
```
