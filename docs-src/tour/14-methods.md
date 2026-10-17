# Methods

A method is a function that belongs to a struct. This chapter covers declaring them, what `self` is, the conventions for making and freeing values, and methods that make a type work with the rest of the language.

## Declaring a method

A method is declared like a function, with the struct's name and a dot in front of its name. Inside it, `self` is a pointer to the value it was called on, and reaches its fields with a dot:

```iris run
struct Counter {
    i64 count
    str name
}

void Counter.add(i64 n) {
    self.count += n
}

str Counter.describe() => if self.count > 10 { "busy" } else { "quiet" }

void main() {
    c := Counter{name: "visits"}
    c.add(4)
    c.add(9)
    print("{c.name}: {c.count}, {c.describe()}")
}
```

```output
visits: 13, busy
```

Since `self` is a pointer, a method can change the value. Called on a variable, it changes the variable. Called on a value that isn't in a variable, like the result of a function, it works on a copy:

```iris run
struct Point {
    i64 x
    i64 y
}

Point origin() => Point{}

void Point.move(i64 dx, i64 dy) {
    self.x += dx
    self.y += dy
}

i64 Point.dist2() => self.x * self.x + self.y * self.y

void main() {
    p := Point{x: 1, y: 1}
    p.move(2, 3)
    print("{p.x}, {p.y}: {p.dist2()}")
    print("{origin().dist2()}")
}
```

```output
3, 4: 25
0
```

Through a pointer to a struct, methods are called the same way: `pp.move(1, 1)` changes the value `pp` points to.

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
    b.add("hello ")
    b.add("there")
    print("[{b.text()}] {b.used} of {b.data.len}")
}
```

```output
[hello there] 11 of 64
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
struct Range {
    i64 from
    i64 to
}

String Range.format() => format("[{self.from}..{self.to})")

(i64, bool) Range.next(i64 *at) {
    v := self.from + *at
    if v >= self.to {
        return (0, false)
    }
    *at += 1
    return (v, true)
}

void main() {
    r := Range{from: 3, to: 6}
    print("going over {r}")
    for n in r {
        print("{n}")
    }
}
```

```output
going over [3..6)
3
4
5
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
