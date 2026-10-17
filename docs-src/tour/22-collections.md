# Collections

`List` keeps values in order and grows as values are added; `Map` keeps a value for each key. Both are in every module without an import. This chapter covers what they do, how loops go over them, and who frees what they hold.

## List

A `List[T]` holds values of type `T` in order. A new list is empty and needs no setup; `free` gives its memory back:

```iris run
void main() {
    List[str] names
    defer names.free()
    names.push("ada")
    names.push("zoe")
    names.push("bob")
    print("{names.len} names: {names.at(0)} ... {names.last()}")
    names.set(1, "joe")
    print("{names.at(1)}")
}
```

```output
3 names: ada ... bob
joe
```

`len` is the number of values. `at(i)` gives the value at index i, from 0, and `set(i, v)` replaces it. An index outside the list stops the program, as with slices.

## Adding and taking out

`push` adds at the end and `pop` takes the last value off and gives it. `insert(i, v)` and `remove(i)` work anywhere in the list, moving the values after it:

```iris run
void show(List[i64] *l) {
    String s = "[".copy()
    for i, v in l.items() {
        scoped String part = format("{if i > 0 { " " } else { "" }}{v}")
        s.append(part)
    }
    s.append("]")
    print(s)
    s.free()
}

void main() {
    List[i64] l
    defer l.free()
    for i in 1..6 {
        l.push(i * 10)
    }
    show(&l)
    last := l.pop()
    l.insert(0, 5)
    gone := l.remove(2)
    print("popped {last}, removed {gone}")
    show(&l)
    l.reverse()
    show(&l)
    l.clear()
    print("{l.len} after clear")
}
```

```output
[10 20 30 40 50]
popped 50, removed 20
[5 10 30 40]
[40 30 10 5]
0 after clear
```

`clear` empties the list and keeps its memory for reuse. `reserve(n)` makes room for n values at once, which saves growing step by step when the count is known ahead.

## Searching and sorting

`contains(v)` says whether a value equal to v is there, and `indexOf(v)` where (-1 when it isn't). They compare with `==`, so they work with values that `==` can compare.

`sort(less)` sorts with a function that says whether its first value goes before its second. Values that are equal keep their order:

```iris run
struct Person {
    str name
    i64 age
}

bool younger(Person a, Person b) => a.age < b.age

void main() {
    List[Person] people
    defer people.free()
    people.push(Person{name: "ada", age: 36})
    people.push(Person{name: "bob", age: 25})
    people.push(Person{name: "cy", age: 30})
    people.sort(younger)
    for p in people {
        print("{p.name} {p.age}")
    }
    List[str] words
    defer words.free()
    words.push("pear")
    words.push("fig")
    print("{words.contains("fig")} {words.indexOf("pear")} {words.indexOf("kiwi")}")
}
```

```output
bob 25
cy 30
ada 36
true 0 -1
```

## The values as a slice

`items()` gives the list's values as a slice, without copying: to pass to a function that takes a slice, or for a loop with indexes. The slice points into the list, so it is valid until the list next grows or is freed:

```iris run
i64 sum(i64[] xs) {
    i64 total
    for x in xs {
        total += x
    }
    return total
}

void main() {
    List[i64] l
    defer l.free()
    l.push(4)
    l.push(5)
    print("{sum(l.items())}")
}
```

```output
9
```

## Map

A `Map[K, V]` keeps a value for each key. `set` adds a key or replaces its value, and `get` gives the value and whether the key was there:

```iris run
void main() {
    Map[str, i64] ages
    defer ages.free()
    ages.set("ada", 36)
    ages.set("bob", 25)
    ages.set("ada", 37)
    age, found := ages.get("ada")
    print("{age} {found} {ages.len}")
    _, there := ages.get("zoe")
    print("{there} {ages.getOr("zoe", 0)} {ages.has("bob")}")
    print("{ages.remove("bob")} {ages.remove("bob")} {ages.len}")
}
```

```output
37 true 2
false 0 true
true false 1
```

`getOr(k, fallback)` gives the value or the fallback, `has(k)` whether the key is there, and `remove(k)` takes it out, giving false when it wasn't there. `keys()` gives the keys as a new `List`, which the caller frees. `clear` takes every key out and keeps the memory.

Keys can be integers, `bool`, enums, pointers or `str`. A `str` key is copied into the map, so the text it came from can change or go away. A map's keys come in no particular order.

## Going over a map

A loop over a map gives each key and its value:

```iris run
void main() {
    Map[str, i64] stock
    defer stock.free()
    stock.set("pears", 4)
    total := 0
    for name, count in stock {
        print("{name}: {count}")
        total += count
    }
    scoped List[str] names = stock.keys()
    print("{names.len} kinds, {total} in all")
}
```

```output
pears: 4
1 kinds, 4 in all
```

For a stable order, the keys go into a list, which is sorted.

## Counting words

```iris run
import str

bool before(str a, str b) => str.compare(a, b) < 0

void main() {
    text := "the cat and the dog and the bird"
    Map[str, i64] counts
    defer counts.free()
    scoped List[str] words = str.fields(text)
    for w in words {
        counts.set(w, counts.getOr(w, 0) + 1)
    }
    scoped List[str] keys = counts.keys()
    keys.sort(before)
    for k in keys {
        print("{k} {counts.getOr(k, 0)}")
    }
}
```

```output
and 2
bird 1
cat 1
dog 1
the 3
```

## Who frees the values

A list or a map holds its values themselves. When the values own memory, like `String`s or structs with a `free` method, something must free them before the list goes. `freeEach` frees each value, then the list:

```iris run
import str

void main() {
    List[String] shouts
    defer shouts.freeEach()
    for w in ["hey", "you"] {
        shouts.push(str.upper(w))
    }
    print("{shouts.at(0)} {shouts.at(1)}")
}
```

```output
HEY YOU
```

`at` and the loop give the list's own values: a `String` from `list.at(i)` belongs to the list, and is freed by `freeEach`, not by the caller. A map with values that own memory frees them in a loop before its own `free`:

```iris run
void main() {
    Map[i64, String] labels
    defer labels.free()
    labels.set(1, "one".copy())
    labels.set(2, "two".copy())
    print("{labels.len} labels")
    for _, v in labels {
        v.free()
    }
}
```

```output
2 labels
```

The `defer` runs last, so the loop frees the values first and the map after them.
