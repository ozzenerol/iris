# Methods

A method is a function that belongs to a struct. This chapter covers declaring them, what `self` is, the conventions for making and freeing values, and methods that make a type work with the rest of the language.

## Declaring a method

A method is declared like a function, with the struct's name and a dot in front of its name. Inside it, `self` is a pointer to the value it was called on, and reaches its fields with a dot:

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

Since `self` is a pointer, a method can change the value. Called on a variable, it changes the variable. Called on a value that isn't in a variable, like the result of a function, it works on a copy:

```iris run
// A TCP sender's window: the bytes it may send before an acknowledgement.
struct Window {
    i64 start // the first byte not acknowledged yet
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

Through a pointer to a struct, methods are called the same way: `wp.ack(1460)` changes the value `wp` points to.

A method can be declared in any file of the module that declares the struct, and in any order. A struct can't have a method and a field with the same name, and each method name is used once.

## Making and freeing

By convention, a function named after the type, in lower case, makes a value of it, and a method `free` gives back what the value holds. `scoped` calls `free` when the block ends:

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

A method that can fail returns `!T` like any function, and a function that makes a value and can fail returns `!Type`.

## Methods the language uses

Some method names make a struct work with parts of the language:

| Method | What it gives |
|---|---|
| `void free()` | `scoped` calls it, and so does `List.freeEach` on each item |
| `String format()` | the struct's text, when it is put into text with braces |
| `(T, bool) next(i64 *at)` | a `for` loop's values, one round at a time |

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

Methods with the names an interface lists let the struct be used where that interface is wanted (see [Interfaces](21-interfaces.html)).

## Methods through embedding

The methods of an embedded struct can be called on the outer one, and get the embedded struct as `self`:

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
