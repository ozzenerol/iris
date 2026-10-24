# Memory

Iris has no garbage collector. Memory that a program asks for, it gives back. This chapter covers where values live, `alloc` and `free`, the statements that make freeing hard to forget (`defer`, `errdefer` and `scoped`), arenas, who frees what, and the checks that catch mistakes.

## Where values live

Numbers, structs and arrays in variables live in their function's frame and go away when it returns. Globals live as long as the program. Copying such a value copies it whole.

Memory that must outlive a function, or whose size is known only at run time, comes from the heap. `alloc` asks for it and `free` gives it back. `String`, `List` and `Map` keep their contents on the heap too, which is why they have a `free` method.

## alloc and free

`alloc(T, n)` gives a slice of `n` items of type `T`, all at zero. `free` takes the slice and gives the memory back:

```iris run
void main() {
    size := 4KiB // could come from a setting
    u8[] buf = alloc(u8, size)

    for i, b in "PING\r\n" {
        buf[i] = b
    }

    print("{buf[0:4]} in a {buf.len} byte buffer")
    free(buf)
}
```

```output
PING in a 4096 byte buffer
```

The slice can be passed around and stored like any other. Whoever holds it last frees it, once. Memory from `alloc` is all zeros, even for a struct whose fields have defaults.

## defer

`defer` runs a statement when the block it is in ends: at the closing brace, at a `return`, at a `break` or `continue`, or when `try` passes an error up. The cleanup goes right after the code that needs it:

```iris run
void serve(bool badRequest) {
    print("connection accepted")
    defer print("connection closed")

    if badRequest {
        print("bad request, leaving early")
        return
    }
    print("request answered")
}

void main() {
    serve(false)
    serve(true)
}
```

```output
connection accepted
request answered
connection closed
connection accepted
bad request, leaving early
connection closed
```

- Several defers run in the reverse order of their lines, so what was set up last is undone first.
- A defer in a loop runs at the end of each round, since the round is a block.
- `defer { ... }` defers a whole block.

```iris run
void main() {
    defer print("socket closed")
    defer {
        print("log flushed")
        print("log closed")
    }

    for i in 0..2 {
        defer print("buffer {i} freed")
        print("sending buffer {i}")
    }
}
```

```output
sending buffer 0
buffer 0 freed
sending buffer 1
buffer 1 freed
log flushed
log closed
socket closed
```

A deferred statement can't `return`, `break` or `continue` out of the block.

## errdefer

`errdefer` runs only when the function returns an error. It undoes work that should stay done only if everything succeeds, such as a half-built value:

```iris run
enum ListenError { inUse }

!void listen(bool portTaken) {
    print("socket opened")
    errdefer print("socket closed again")
    defer print("attempt logged")

    if portTaken {
        return error(ListenError.inUse, "port 8080 is taken")
    }
    print("listening")
}

void main() {
    listen(false) catch e { }
    print("--")
    listen(true) catch e { }
}
```

```output
socket opened
listening
attempt logged
--
socket opened
attempt logged
socket closed again
```

## scoped

`scoped` on a declaration calls the value's `free` method when the block ends. It equals the declaration followed by `defer x.free()`:

```iris run
import str

void main() {
    scoped String method = str.upper("get")
    scoped List[u16] ports

    ports.push(80)
    ports.push(443)
    print("{method} on {ports.len} ports")
}
```

```output
GET on 2 ports
```

The type can come from the value, as with `:=`: `scoped t := term.table(...)`. `scoped` works with any type that has a `free` method taking nothing: `String`, `List`, `Map`, the library's types and the program's own structs.

## Arenas

An `Arena` gives out memory for many values that are freed together, such as everything one request needs.

- `a.alloc(T, n)` gives `n` items of `T`, all at zero, as `alloc` does. They are never freed one by one.
- `a.copy(text)` puts a copy of some text in the arena.
- `a.reset()` gives back everything at once and keeps a block of memory to give out again.
- `a.free()` gives back all of it.

```iris run
struct Header {
    str name
    str value
}

i64 handle(Arena *mem, str[] lines) {
    Header[] headers = mem.alloc(Header, lines.len)

    for i, line in lines {
        name, value, _ := line.cut(": ")
        headers[i] = .{name: mem.copy(name), value: mem.copy(value)}
    }

    return headers.len
}

void main() {
    scoped Arena mem
    requests := [["Host: example.com", "Accept: */*"], ["Host: api.lan", "Accept: text/html"]]

    for r in requests {
        n := handle(&mem, r)
        print("{n} headers in {mem.used()} bytes")
        mem.reset()
    }
}
```

```output
2 headers in 88 bytes
2 headers in 90 bytes
```

The arena takes memory from the heap in blocks of `blockSize` bytes, 64 KiB unless the literal sets another: `Arena{blockSize: 1MiB}`. A value bigger than a quarter of a block gets a block of its own.

An arena can't be copied, since the copy would give its memory back a second time. Functions take a pointer to it. Memory from an arena is never passed to `free` and is not used after `reset` or `free`. A debug build reports a use after either.

## Who frees what

The standard library follows these conventions throughout:

- A function that returns a `String`, a `List`, a `Map` or a struct with a `free` method gives the caller a new value, which the caller frees: `str.upper`, `format`, `fs.readAll`, `json.encode`.
- A `str` or a slice that a function gives back points into something else: into its argument (`str.trim`), or into a value that still owns it (`list.items()`). It is valid as long as that is, and is never freed.
- A `List[String]`, or a list of structs with `free`, holds values that must be freed too. `freeEach` frees each one, then the list.
- A struct that owns memory has a `free` method that frees it all. One that only looks at memory owned elsewhere has none.

```iris run
import str

// A peer, and the protocols it offered.
struct Peer {
    String host
    List[String] protocols
}

void Peer.free() {
    self.host.free()
    self.protocols.freeEach()
}

Peer makePeer(str host) {
    Peer p = Peer{host: str.lower(host)}
    p.protocols.push("http/1.1".copy())
    return p
}

void main() {
    scoped p := makePeer("API.Example.com")
    p.protocols.push("h2".copy())

    print("{p.host} offers {p.protocols.len} protocols")
}
```

```output
api.example.com offers 2 protocols
```

Structs marked `@json`, `@env` or `@flags` get a generated `free` method that frees their Strings and slices (see [Attributes](27-attributes.html)).

## Checks in debug builds

A debug build, which `iris run` makes, stops the program with a message and the location when it finds:

- an index outside an array, slice or `str`
- an integer that overflows, or a division by zero
- a conversion that loses a value
- a `null` pointer used

It also finds memory used after it was freed and memory freed twice, with AddressSanitizer from `clang`. The report shows the Iris lines where the memory was used, freed and allocated:

```output
ERROR: AddressSanitizer: heap-use-after-free
WRITE of size 1 at 0x502000000010 thread T0
    #0 in f.main.main main.iris:4:5
```

| Build | Checks |
|---|---|
| `debug` | all of them |
| `release-safe` | indexes and slices, and division by zero |
| `release-fast` | none |

A function marked `@unchecked` has no checks in any build. It is meant for code that has been measured and needs the speed:

```iris
@unchecked
u32 checksum(u8[] data) {
    u32 sum = 0
    for b in data {
        sum = sum * 31 + u32(b)
    }
    return sum
}
```
