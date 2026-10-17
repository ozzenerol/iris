# Interfaces

An interface is a list of methods. A struct with those methods has the interface; nothing more needs to be written. Interfaces are used in two ways: as bounds on type parameters, worked out when compiling, and as `dyn` values, worked out when the program runs.

## Declaring an interface

An interface lists the methods a type must have, with their types:

```iris
interface Shape {
    f64 area()
    String format()
}
```

Any struct with an `area` method that takes nothing and gives an `f64`, and a `format` method that gives a `String`, has `Shape`. Only structs have methods, so only structs have interfaces.

## Bounds

A type parameter written `T: Shape` takes only types that have `Shape`, and the generic code can then call its methods:

```iris run
interface Shape {
    f64 area()
}

struct Square {
    f64 side
}

f64 Square.area() => self.side * self.side

struct Circle {
    f64 radius
}

f64 Circle.area() => 3.0 * self.radius * self.radius

T bigger[T: Shape](T a, T b) => if a.area() >= b.area() { a } else { b }

f64 areaOf[T: Shape](T s) => s.area()

void main() {
    big := bigger(Square{side: 2}, Square{side: 3})
    print("{big.side} {areaOf(Circle{radius: 1})}")
}
```

```output
3 3
```

A call with a type that lacks a method is an error that names the missing method. Each use is compiled for its own type, so a bound costs nothing when the program runs.

## Self

In an interface, `Self` stands for the type that has the interface. It lets a method take or give a value of the same type:

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

T max[T: Ordered](T a, T b) => if a.compare(b) >= 0 { a } else { b }

void main() {
    v := max(Version{major: 1, minor: 2}, Version{major: 1, minor: 5})
    print("{v.major}.{v.minor}")
}
```

```output
1.5
```

## dyn

A `dyn Shape` value holds any type with the interface, chosen when the program runs: the methods are looked up through a table that comes with the value. It is made from a pointer to a struct, and works where the types aren't known ahead, like a list of different kinds of things:

```iris run
interface Shape {
    f64 area()
    str name()
}

struct Square {
    f64 side
}

f64 Square.area() => self.side * self.side
str Square.name() => "square"

struct Circle {
    f64 radius
}

f64 Circle.area() => 3.0 * self.radius * self.radius
str Circle.name() => "circle"

void describe(dyn Shape s) => print("a {s.name()} of {s.area()}")

void main() {
    sq := Square{side: 2}
    ci := Circle{radius: 1}
    describe(&sq)
    List[dyn Shape] all
    defer all.free()
    all.push(&sq)
    all.push(&ci)
    f64 total = 0
    for s in all {
        total += s.area()
    }
    print("{all.len} shapes, {total} in all")
}
```

```output
a square of 4
2 shapes, 7 in all
```

A `dyn` value holds a pointer, not a copy: the struct must live as long as the `dyn` value is used. Its type must have each of the interface's methods with the same types. An interface with `Self` can't be used with `dyn`, since the type behind it isn't known when compiling.

## Which one to use

Bounds are the default: they cost nothing when the program runs, and the compiler checks each use with its own type. `dyn` is for values whose types are known only when the program runs, such as handlers of different kinds kept in one list, or plug-ins chosen by a setting. A call through `dyn` costs one lookup in the value's table.
