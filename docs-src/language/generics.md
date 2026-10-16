# Generics

Writing a struct or function once and using it with many types.

## Generic structs

Type parameters are written in square brackets after the name:

```iris
struct List[T] {
    T *items
    i64 len
    i64 cap
}

struct Pair[A, B] {
    A first
    B second
}
```

Methods of a generic struct repeat the parameters:

```iris
void List[T].push(T value) {
    if self.len == self.cap {
        self.grow()
    }
    self.items[self.len] = value
    self.len += 1
}
```

A generic type is used with its types filled in:

```iris
List[i64] numbers
numbers.push(4)

p := Pair[str, i64]{first: "age", second: 30}
```

## Generic functions

```iris
T first[T](T[] items) => items[0]
```

The types are worked out from the arguments, so they rarely need to be written:

```iris
n := first(counts)           // T is i64
s := first(names)            // T is str
l := List.of(1, 2, 3)        // List[i64]
```

When there is nothing to work them out from, they are written in brackets:

```iris
names := List[str].new()
```

## Interfaces

An interface lists methods. A type has an interface when it has those methods. Nothing else needs to be declared.

```iris
interface Ordered {
    i64 compare(Self other)
}
```

`Self` is the type that has the interface.

An interface can limit what a type parameter accepts. Inside the function, those methods can be called:

```iris
T max[T: Ordered](T a, b) => if a.compare(b) >= 0 { a } else { b }
```

Calling `max` with a type that has no `compare` method is a compile error that names the missing method.

## dyn

Normally every call is worked out when compiling. A `dyn` parameter accepts any type with the interface and picks the method when the program runs. Use it when the type is not known until then, like a list of handlers of different types:

```iris
interface Writer {
    !i64 write(u8[] data)
}

!void log(dyn Writer w, str msg) {
    try w.write(msg)
}

try log(&file, "started")
try log(&conn, "started")
```

## How it is compiled

Each generic function or struct is compiled once for each set of types it is used with. The result is as fast as writing a separate copy for each type by hand.
