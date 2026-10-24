## Lists, maps and sets

`List`, `Map` and `Set` need no setup. A new one is empty, and `free` gives its memory back. A `str` or `String` key is copied into a map or a set:

```iris run
void main() {
    List[i64] sizes
    defer sizes.free()
    for n in [512, 64, 4096] {
        sizes.push(n)
    }
    sort(sizes.items())

    Map[str, i64] ports
    defer ports.free()
    ports.set("https", 443)

    Set[str] seen
    defer seen.free()
    _ = seen.add("10.0.0.5")

    print("{sizes.at(0)} {ports.getOr("https", 0)} {seen.has("10.0.0.5")}")
}
```

```output
64 443 true
```

## Queues and ordered maps

`Deque` adds and takes at both ends. `Heap` and `SortedMap` are made with a `less` function that sets their order:

```iris run
bool lower(i64 a, i64 b) => a < b

void main() {
    scoped Deque[i64] d
    d.push(2)
    d.pushFront(1)

    scoped Heap[i64] h = Heap[i64]{less: lower}
    h.push(30)
    h.push(10)

    scoped SortedMap[i64, str] m = SortedMap[i64, str]{less: lower}
    m.set(404, "Not Found")
    m.set(200, "OK")
    k, v, _ := m.first()

    print("{d.popFront()} {h.pop()} {k} {v}")
}
```

```output
1 10 200 OK
```

## Slices

`sort`, `reverse`, `has`, `indexOf`, `min`, `max` and `binarySearch` take any slice, and a `List` through `items()`. `sortBy` and `binarySearchBy` take a `less` function for values that `<` can't compare.
