# Collections

`List` keeps values in order and grows as values are added; `Map` keeps a value for each key, and `Set` keeps each value once. `Deque` is a queue open at both ends, `Heap` gives back the value that goes first, and `SortedMap` keeps its keys in order. All of them, and the functions on slices, are in every module without an import. This chapter covers what they do, how loops go over them, and who frees what they hold.

## List

A `List[T]` holds values of type `T` in order. A new list is empty and needs no setup; `free` gives its memory back:

```iris run
void main() {
    List[str] hosts
    defer hosts.free()
    hosts.push("10.0.0.1")
    hosts.push("10.0.0.2")
    hosts.push("10.0.0.3")
    print("{hosts.len} hosts: {hosts.at(0)} ... {hosts.last()}")
    hosts.set(1, "10.0.0.9")
    print("{hosts.at(1)}")
}
```

```output
3 hosts: 10.0.0.1 ... 10.0.0.3
10.0.0.9
```

`len` is the number of values. `at(i)` gives the value at index i, from 0, and `set(i, v)` replaces it. An index outside the list stops the program, as with slices.

## Adding and taking out

`push` adds at the end and `pop` takes the last value off and gives it. `insert(i, v)` and `remove(i)` work anywhere in the list, moving the values after it:

```iris run
void show(List[u16] *ports) {
    String s = "[".copy()
    for i, p in ports.items() {
        scoped String part = format("{if i > 0 { " " } else { "" }}{p}")
        s.append(part)
    }
    s.append("]")
    print(s)
    s.free()
}

void main() {
    List[u16] ports
    defer ports.free()
    for i in 0..5 {
        ports.push(u16(8080 + i))
    }
    show(&ports)
    last := ports.pop()
    ports.insert(0, 22)
    gone := ports.remove(2)
    print("popped {last}, removed {gone}")
    show(&ports)
    ports.reverse()
    show(&ports)
    ports.clear()
    print("{ports.len} after clear")
}
```

```output
[8080 8081 8082 8083 8084]
popped 8084, removed 8081
[22 8080 8082 8083]
[8083 8082 8080 22]
0 after clear
```

`clear` empties the list and keeps its memory for reuse. `reserve(n)` makes room for n values at once, which saves growing step by step when the count is known ahead.

## Searching and sorting

`contains(v)` says whether a value equal to v is there, and `indexOf(v)` where (-1 when it isn't). They compare with `==`, so they work with values that `==` can compare.

`sort(less)` sorts with a function that says whether its first value goes before its second. Values that are equal keep their order:

```iris run
struct Upstream {
    str addr
    Duration latency
}

bool faster(Upstream a, Upstream b) => a.latency < b.latency

void main() {
    List[Upstream] ups
    defer ups.free()
    ups.push(Upstream{addr: "10.0.0.11:8080", latency: 36ms})
    ups.push(Upstream{addr: "10.0.0.12:8080", latency: 4ms})
    ups.push(Upstream{addr: "10.0.0.13:8080", latency: 12ms})
    ups.sort(faster)
    for u in ups {
        print("{u.addr} {u.latency}")
    }
    List[str] protocols
    defer protocols.free()
    protocols.push("http/1.1")
    protocols.push("h2")
    print("{protocols.contains("h2")} {protocols.indexOf("http/1.1")} {protocols.indexOf("h3")}")
}
```

```output
10.0.0.12:8080 4ms
10.0.0.13:8080 12ms
10.0.0.11:8080 36ms
true 0 -1
```

## The values as a slice

`items()` gives the list's values as a slice, without copying: to pass to a function that takes a slice, or for a loop with indexes. The slice points into the list, so it is valid until the list next grows or is freed:

```iris run
i64 total(i64[] sizes) {
    i64 sum
    for s in sizes {
        sum += s
    }
    return sum
}

void main() {
    List[i64] reads
    defer reads.free()
    reads.push(4096)
    reads.push(1200)
    print("{total(reads.items())} bytes read")
}
```

```output
5296 bytes read
```

## Map

A `Map[K, V]` keeps a value for each key. `set` adds a key or replaces its value, and `get` gives the value and whether the key was there:

```iris run
void main() {
    // open connections for each client address
    Map[str, i64] conns
    defer conns.free()
    conns.set("10.0.0.5", 3)
    conns.set("10.0.0.9", 1)
    conns.set("10.0.0.5", 4)
    n, found := conns.get("10.0.0.5")
    print("{n} {found} {conns.len}")
    _, there := conns.get("10.0.0.7")
    print("{there} {conns.getOr("10.0.0.7", 0)} {conns.has("10.0.0.9")}")
    print("{conns.remove("10.0.0.9")} {conns.remove("10.0.0.9")} {conns.len}")
}
```

```output
4 true 2
false 0 true
true false 1
```

`getOr(k, fallback)` gives the value or the fallback, `has(k)` whether the key is there, and `remove(k)` takes it out, giving false when it wasn't there. `keys()` gives the keys as a new `List`, which the caller frees. `clear` takes every key out and keeps the memory.

Keys can be integers, `bool`, enums, pointers, `str` or `String`. A `str` or `String` key is copied into the map, so the text it came from can change or go away, and the caller still frees its own `String`. The keys from `keys()` and from a loop belong to the map: free the list, not the keys in it. A map's keys come in no particular order.

## Going over a map

A loop over a map gives each key and its value:

```iris run
void main() {
    Map[str, i64] hits
    defer hits.free()
    hits.set("/health", 4)
    total := 0
    for path, count in hits {
        print("{path}: {count}")
        total += count
    }
    scoped List[str] paths = hits.keys()
    print("{paths.len} paths, {total} requests")
}
```

```output
/health: 4
1 paths, 4 requests
```

For a stable order, the keys go into a list, which is sorted.

## Set

A `Set[T]` keeps each value once. `add` puts a value in and gives false when it was there already, `has` says whether a value is there, and `remove` takes it out. A loop over a set gives each value, in no particular order:

```iris run
void main() {
    Set[str] blocked
    defer blocked.free()
    for addr in ["10.0.0.5", "10.0.0.9", "10.0.0.5"] {
        if !blocked.add(addr) {
            print("{addr} is blocked already")
        }
    }
    print("{blocked.len} blocked, {blocked.has("10.0.0.9")} {blocked.has("10.0.0.7")}")
    _ = blocked.remove("10.0.0.9")
    for addr in blocked {
        print("still blocked: {addr}")
    }
}
```

```output
10.0.0.5 is blocked already
2 blocked, true false
still blocked: 10.0.0.5
```

A set's values follow the rules of a map's keys: integers, `bool`, enums, pointers, `str` or `String`, and a `str` or `String` is copied into the set. `values()` gives them as a new `List`, which the caller frees. `clear` takes every value out and keeps the memory.

A `Set[String]` works the same way. The set keeps its own copy, so the `String` that was added is still the caller's to free:

```iris run
void main() {
    Set[String] users
    defer users.free()
    for name in ["ana", "bo", "ana"] {
        String s = name.copy()
        print("{s} is new: {users.add(s)}")
        s.free()
    }
    print("{users.len} users")
}
```

```output
ana is new: true
bo is new: true
ana is new: false
2 users
```

## Requests per client

An access log has a line for each request, with the client's address first. A map counts the lines of each address, and the sorted keys print them in order:

```iris run
import str

bool before(str a, str b) => str.compare(a, b) < 0

void main() {
    log := """
        10.0.0.5 GET / 200
        10.0.0.9 GET /login 200
        10.0.0.5 POST /login 302
        10.0.0.7 GET / 200
        10.0.0.5 GET /admin 403
        """
    Map[str, i64] counts
    defer counts.free()
    scoped List[str] lines = str.split(log, "\n")
    for line in lines {
        ip, _, found := str.cut(line, " ")
        continue if !found
        counts.set(ip, counts.getOr(ip, 0) + 1)
    }
    scoped List[str] ips = counts.keys()
    ips.sort(before)
    for ip in ips {
        print("{ip} {counts.getOr(ip, 0)}")
    }
}
```

```output
10.0.0.5 3
10.0.0.7 1
10.0.0.9 1
```

## Who frees the values

A list or a map holds its values themselves. When the values own memory, like `String`s or structs with a `free` method, something must free them before the list goes. `freeEach` frees each value, then the list:

```iris run
import str

void main() {
    List[String] headers
    defer headers.freeEach()
    for h in ["Content-Type", "X-Request-Id"] {
        headers.push(str.lower(h))
    }
    print("{headers.at(0)} {headers.at(1)}")
}
```

```output
content-type x-request-id
```

`at` and the loop give the list's own values: a `String` from `list.at(i)` belongs to the list, and is freed by `freeEach`, not by the caller. A map with values that own memory frees them in a loop before its own `free`:

```iris run
void main() {
    Map[i64, String] reasons
    defer reasons.free()
    reasons.set(200, "OK".copy())
    reasons.set(404, "Not Found".copy())
    text, _ := reasons.get(404)
    print("{reasons.len} reasons, 404 is {text}")
    for _, v in reasons {
        v.free()
    }
}
```

```output
2 reasons, 404 is Not Found
```

The `defer` runs last, so the loop frees the values first and the map after them.

## Functions on slices

`sort`, `reverse`, `contains`, `indexOf`, `min`, `max` and `binarySearch` work on any slice. A list passes its values with `items()`, and the list itself changes:

```iris run
void main() {
    i64[] ms = [36, 4, 12, 90, 7]
    sort(ms)
    print("{ms[0]} {ms[4]}, min {min(ms)}, max {max(ms)}")
    at, found := binarySearch(ms, 12)
    where, there := binarySearch(ms, 50)
    print("12 at {at} {found}, 50 would go at {where} {there}")
    print("{contains(ms, 90)} {indexOf(ms, 7)} {indexOf(ms, 8)}")
    List[u16] ports
    defer ports.free()
    for p in [443, 22, 8080] {
        ports.push(u16(p))
    }
    sort(ports.items())
    reverse(ports.items())
    print("{ports.at(0)} {ports.at(1)} {ports.at(2)}")
}
```

```output
4 90, min 4, max 90
12 at 2 true, 50 would go at 4 false
true 1 -1
8080 443 22
```

`sort`, `min`, `max` and `binarySearch` compare with `<`, so they take numbers, `Duration`s and text. `binarySearch` needs a sorted slice; it gives where the value is, or where it would go to keep the slice sorted, and whether it is there. `contains` and `indexOf` compare with `==`. For other values, `sortBy(xs, less)` sorts with a function that says whether its first value goes before its second, as `List.sort` does, and `binarySearchBy(xs, v, less)` searches a slice sorted that way. Both sort in place, with no memory of their own, and values that are equal may change places; `List.sort` keeps them in their order. `min` or `max` of an empty slice stops the program.

## Deque

A `Deque[T]` is a queue open at both ends. `push` and `pop` add and take at the back, `pushFront` and `popFront` at the front, and each takes the same time however many values there are. `first` and `last` give the values at the ends, and `at(i)` the one at i, counting from the front:

```iris run
void main() {
    // jobs waiting for a worker; a retry goes to the front
    Deque[str] jobs
    defer jobs.free()
    jobs.push("resize 1.png")
    jobs.push("resize 2.png")
    jobs.push("resize 3.png")
    next := jobs.popFront()
    print("working on {next}")
    jobs.pushFront(next)
    print("{jobs.len} waiting, first {jobs.first()}, last {jobs.last()}")
    for j in jobs {
        print(j)
    }
}
```

```output
working on resize 1.png
3 waiting, first resize 1.png, last resize 3.png
resize 1.png
resize 2.png
resize 3.png
```

A loop goes from the front to the back. Taking a value from an empty deque stops the program, as `List.pop` does. `clear`, `free` and `freeEach` work as they do on a `List`.

## Heap

A `Heap[T]` gives back its values in order, the one that goes first each time, without sorting all of them. The order comes from a `less` function, given when the heap is made:

```iris run
struct Timer {
    str name
    Duration due
}

bool sooner(Timer a, Timer b) => a.due < b.due

void main() {
    Heap[Timer] timers = Heap[Timer]{less: sooner}
    defer timers.free()
    timers.push(Timer{name: "health check", due: 30s})
    timers.push(Timer{name: "flush logs", due: 5s})
    timers.push(Timer{name: "rotate keys", due: 1h})
    print("next: {timers.peek().name}")
    for timers.len > 0 {
        t := timers.pop()
        print("{t.due} {t.name}")
    }
}
```

```output
next: flush logs
5s flush logs
30s health check
1h0m0s rotate keys
```

`push` adds a value and `pop` takes out the one that goes first; both take time that grows with the logarithm of the count. `peek` gives that value without taking it out, and `len` is the count. With a `less` that says `a > b`, the largest comes first. A heap without a `less` stops the program when a value is pushed.

## SortedMap

A `SortedMap[K, V]` is a map that keeps its keys in order. Like a heap, it is made with a `less` function, which says whether one key goes before another. A loop goes over the keys in order, `first` and `last` give the ends, and `range(from, to)` gives the keys from `from` up to `to`, without `to`:

```iris run
import str

bool before(str a, str b) => str.compare(a, b) < 0

void main() {
    // bytes sent to each client, by name
    SortedMap[str, i64] sent = SortedMap[str, i64]{less: before}
    defer sent.free()
    sent.set("web-3", 900)
    sent.set("api-1", 1200)
    sent.set("web-1", 300)
    sent.set("db-1", 50)
    for name, n in sent {
        print("{name} {n}")
    }
    k, v, _ := sent.first()
    print("first {k} {v}")
    for name, _ in sent.range("web", "wf") {
        print("a web host: {name}")
    }
}
```

```output
api-1 1200
db-1 50
web-1 300
web-3 900
first api-1 1200
a web host: web-1
a web host: web-3
```

`set`, `get`, `getOr`, `has` and `remove` work as on a `Map`, and take time that grows with the logarithm of the count. Two keys where neither goes before the other are the same key. Keys can be what a `Map` takes, and a `str` or `String` key is copied into the map. The map must not change during a loop over it.
