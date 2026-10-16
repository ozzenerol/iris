# Collections

`List` and `Map`, which every module can use without an import, and how a loop goes over them.

## List

A `List[T]` holds values in order and grows as values are added. A new list is empty:

```iris
List[str] names
defer names.free()

names.push("ada")
names.push("zoe")
print("{names.len} {names.at(0)} {names.last()}")
```

| Method | What it does |
|---|---|
| `push(v)` | adds a value at the end |
| `pop()` | takes the last value off and gives it |
| `at(i)`, `set(i, v)` | the value at i, from 0, and replacing it |
| `last()` | the last value |
| `insert(i, v)`, `remove(i)` | puts a value at i, or takes it out, moving the others |
| `items()` | the values as a slice, valid until the list changes size |
| `sort(less)` | sorts with a function that says whether a goes before b; equal values keep their order |
| `reverse()` | turns the order around |
| `reserve(n)` | makes room for n values at once |
| `clear()` | empties the list and keeps its memory |
| `free()` | gives the memory back |

`len` is the number of values. An index outside the list stops the program, as with slices.

The list holds the values themselves. A list of `String` holds Strings that something still has to free.

## Map

A `Map[K, V]` holds a value for each key:

```iris
Map[str, i64] ages
defer ages.free()

ages.set("ada", 36)
age, found := ages.get("ada")
n := ages.getOr("zoe", 0)
```

| Method | What it does |
|---|---|
| `set(k, v)` | sets the value of k, adding k if it isn't there |
| `get(k)` | the value of k and whether k is there |
| `getOr(k, fallback)` | the value of k, or fallback |
| `has(k)` | whether k is there |
| `remove(k)` | takes k out; false when it wasn't there |
| `clear()` | takes every key out and keeps the memory |
| `free()` | gives the memory back |

Keys can be integers, `bool`, enums, pointers or `str`. A `str` key is copied into the map, so the text it came from can change or go away. `len` is the number of keys.

## Going over them

`for` goes over a list's values and a map's keys and values. A map gives them in no particular order:

```iris
for name in names {
    print(name)
}

for name, age in ages {
    print("{name} is {age}")
}
```

Any struct can be used this way. It needs a method `next(i64 *at)` that gives the next value and a bool, false when there are no more. `at` starts at 0 and the method moves it on:

```iris
struct Countdown {
    i64 from
}

(i64, bool) Countdown.next(i64 *at) {
    if *at >= self.from {
        return (0, false)
    }
    *at += 1
    return (self.from - *at + 1, true)
}

for n in c {
    print("{n}")
}
```

With two values before the bool, the loop takes two names, as with a map.

A complete program:

```iris run
import str

bool shorter(str a, str b) => a.len < b.len

void main() {
    text := "the cat and the dog and the bird"
    Map[str, i64] counts
    defer counts.free()
    List[str] words
    defer words.free()
    rest := text
    for {
        word, after, more := str.cut(rest, " ")
        if !counts.has(word) {
            words.push(word)
        }
        counts.set(word, counts.getOr(word, 0) + 1)
        if !more {
            break
        }
        rest = after
    }
    words.sort(shorter)
    for w in words {
        print("{w} {counts.getOr(w, 0)}")
    }
}
```

```output
the 3
cat 1
and 2
dog 1
bird 1
```
