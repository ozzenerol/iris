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
n := first(counts)           // counts is i64[], so T is i64
c := first("hello")          // str is u8[], so T is u8
```

When there is nothing to work them out from, or a different type is wanted, they are written in brackets after the name:

```iris
x := first[u8](bytes)
T zero[T]() {
    T value
    return value
}
z := zero[i64]()
```

A generic struct always has its types written out, and starts empty like any struct:

```iris
List[str] names
pairs := List[Pair[str, i64]]{}
```

A complete program:

```iris run
struct Pair[A, B] {
    A first
    B second
}

Pair[B, A] swap[A, B](Pair[A, B] p) => Pair[B, A]{first: p.second, second: p.first}

T sum[T](T[] xs) {
    T total
    for x in xs {
        total += x
    }
    return total
}

void main() {
    p := swap(Pair[str, i64]{first: "age", second: 30})
    print("{p.first} {p.second}")
    i64[3] counts = [1, 2, 3]
    print("{sum(counts)}")
}
```

```output
30 age
6
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

Calling `max` with a type that has no `compare` method is a compile error that names the missing method. Only structs have methods, so only structs can satisfy an interface.

```iris run
interface Ordered {
    i64 compare(Self other)
}

struct Version {
    i64 major
    i64 minor
}

i64 Version.compare(Version other) {
    if self.major != other.major {
        return self.major - other.major
    }
    return self.minor - other.minor
}

T max[T: Ordered](T a, b) => if a.compare(b) >= 0 { a } else { b }

void main() {
    v := max(Version{major: 1, minor: 2}, Version{major: 1, minor: 5})
    print("{v.major}.{v.minor}")
}
```

```output
1.5
```

## dyn

Normally every call is worked out when compiling. A `dyn` parameter accepts any type with the interface and picks the method when the program runs. Use it when the type is not known until then, like a list of handlers of different types:

```iris
interface Writer {
    !i64 write(u8[] data)
}

!void log(dyn Writer w, str msg) {
    _ = try w.write(msg)
}

try log(&file, "started")
try log(&conn, "started")
```

A `dyn` value is made from a pointer to a struct: `&file`. Its type must have every method of the interface, with the same types. `Self` can't be used in an interface that is used with `dyn`.

## How it is compiled

Each generic function or struct is compiled once for each set of types it is used with. The result is as fast as writing a separate copy for each type by hand.
