# Generics

A generic function or struct is written once, with type parameters, and used with many types. This chapter covers both, how the types are worked out, and how generic code is compiled.

## Generic functions

Type parameters go in square brackets after the function's name. Inside the function they are used like any type:

```iris run
T first[T](T[] items) => items[0]

T peak[T](T[] samples) {
    T best = samples[0]
    for s in samples {
        if s > best {
            best = s
        }
    }
    return best
}

void main() {
    print("{first(["1.1.1.1", "8.8.8.8"])} {first([8080, 8081])}")
    print("{peak([12, 48, 9])} {peak([0.4, 0.93])} {peak([20ms, 1s, 250ms])}")
}
```

```output
1.1.1.1 8080
48 0.93 1s
```

## Working out the types

The types are worked out from the arguments, so calls rarely name them. When there is nothing to work them out from, or another type is wanted, they are given in brackets after the name:

```iris run
T zero[T]() {
    T v
    return v
}

T twice[T](T x) => x + x

void main() {
    print("{zero[i64]()} [{zero[str]()}] {zero[bool]()}")
    print("{twice(1460)} {twice(250ms)} {twice[f64](0.25)}")
}
```

```output
0 [] false
2920 500ms 0.5
```

## What a generic function may do

A generic function is checked with each set of types it is used with, as if it had been written out for them. Code that doesn't make sense for one of them is an error that names the call that used it:

```iris error
T peak[T](T[] samples) {
    T best = samples[0]
    for s in samples {
        if s > best {    // error: '>' can't compare bool values
            best = s
        }
    }
    return best
}

peak([true, false])   // error: peak[bool], made here, has the errors above
```

To say ahead which types a parameter takes, and give the function methods to call on it, a type parameter can have an interface as its bound (see [Interfaces](21-interfaces.html)).

## Generic structs

A struct's type parameters go after its name. A generic struct is always used with its types written out:

```iris run
struct Pair[A, B] {
    A first
    B second
}

Pair[B, A] swap[A, B](Pair[A, B] p) => Pair[B, A]{first: p.second, second: p.first}

void main() {
    p := Pair[str, u16]{first: "db.lan", second: 5432}
    q := swap(p)
    print("{p.first}:{p.second} / {q.first}:{q.second}")
}
```

```output
db.lan:5432 / 5432:db.lan
```

`List[T]` and `Map[K, V]` from the standard library are generic structs like these (see [Collections](22-collections.html)).

## Methods of generic structs

A method of a generic struct repeats the type parameters after the struct's name:

```iris run
struct Stack[T] {
    List[T] items
}

void Stack[T].push(T v) => self.items.push(v)

(T, bool) Stack[T].pop() {
    if self.items.len == 0 {
        T none
        return (none, false)
    }
    return (self.items.pop(), true)
}

i64 Stack[T].len() => self.items.len

void Stack[T].free() => self.items.free()

void main() {
    // idle connections, the last one put back is used first
    scoped Stack[str] idle
    idle.push("10.0.0.11:5432")
    idle.push("10.0.0.12:5432")
    conn, ok := idle.pop()
    print("{conn} {ok}, {idle.len()} left")
    scoped Stack[i32] freeFds
    _, any := freeFds.pop()
    print("{any}")
}
```

```output
10.0.0.12:5432 true, 1 left
false
```

A method is checked for a set of types only when the program uses it with them. A method can need more of its types than the others do, and the struct still works with types that only the other methods suit. `List[T].contains` compares values with `==`; a list of a struct that can't be compared works until `contains` is called on it.

## Methods with types of their own

A method can have type parameters of its own, written after its name, as a generic function's are. Its types are worked out from the arguments of each call:

```iris run
struct Metrics {
    i64 reported
}

void Metrics.report[T](str name, T value) {
    self.reported++
    print("{name} = {value}")
}

struct Batch[T] {
    List[T] items
}

void Batch[T].free() => self.items.free()

// A new batch with f applied to each item.
Batch[U] Batch[T].map[U](U(T) f) {
    Batch[U] out
    for x in self.items.items() {
        out.items.push(f(x))
    }
    return out
}

i64 kib(i64 bytes) => bytes / 1024
bool big(i64 bytes) => bytes > 1MiB

void main() {
    Metrics m
    m.report("open connections", 12)
    m.report("load", 0.75)
    m.report("region", "eu-west")
    scoped Batch[i64] sizes
    sizes.items.push(4096)
    sizes.items.push(3MiB)
    scoped Batch[i64] k = sizes.map(kib)
    scoped Batch[bool] b = sizes.map(big)
    print("{k.items.at(1)} KiB, {b.items.at(0)} {b.items.at(1)}, {m.reported} reported")
}
```

```output
open connections = 12
load = 0.75
region = eu-west
3072 KiB, false true, 3 reported
```

Every type of such a method must show up in its parameters, so that a call says what it is. A method like `U Box.make[U]()` can't be called, since nothing in `b.make()` gives `U`; a generic function is the way to write that one.

## How generics are compiled

Each generic function and struct is compiled once for each set of types it is used with. The copy for `i64` works on `i64`s directly, with no boxing and no lookups when the program runs, so it is as fast as code written by hand for that type. The cost is in compiling: each set of types adds a copy of the code.
