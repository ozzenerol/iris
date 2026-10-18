# Interfaces

An interface is a list of methods. A struct with those methods has the interface; nothing more needs to be written. Interfaces are used in two ways: as bounds on type parameters, worked out when compiling, and as `dyn` values, worked out when the program runs.

## Declaring an interface

An interface lists the methods a type must have, with their types:

```iris
interface Link {
    i64 mtu()
    String format()
}
```

Any struct with an `mtu` method that takes nothing and gives an `i64`, and a `format` method that gives a `String`, has `Link`. Only structs have methods, so only structs have interfaces.

## Bounds

A type parameter written `T: Link` takes only types that have `Link`, and the generic code can then call its methods:

```iris run
// A network link, and the largest packet it carries.
interface Link {
    i64 mtu()
}

struct Ethernet {
    bool jumbo
}

i64 Ethernet.mtu() => if self.jumbo { 9000 } else { 1500 }

struct Tunnel {
    i64 overhead
}

i64 Tunnel.mtu() => 1500 - self.overhead

T roomier[T: Link](T a, T b) => if a.mtu() >= b.mtu() { a } else { b }

// The TCP payload a packet on the link can carry.
i64 payload[T: Link](T l) => l.mtu() - 40

void main() {
    big := roomier(Ethernet{}, Ethernet{jumbo: true})
    print("{big.jumbo} {payload(Tunnel{overhead: 80})}")
}
```

```output
true 1380
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

A `dyn Link` value holds any type with the interface, chosen when the program runs: the methods are looked up through a table that comes with the value. It is made from a pointer to a struct, and works where the types aren't known ahead, like a list of different kinds of things. Here the links on a network path are of different kinds, and the largest packet that crosses all of them is the smallest MTU:

```iris run
interface Link {
    i64 mtu()
    str name()
}

struct Ethernet {
    bool jumbo
}

i64 Ethernet.mtu() => if self.jumbo { 9000 } else { 1500 }
str Ethernet.name() => "ethernet"

struct Tunnel {
    i64 overhead
}

i64 Tunnel.mtu() => 1500 - self.overhead
str Tunnel.name() => "tunnel"

void describe(dyn Link l) => print("{l.name()} carries {l.mtu()} bytes")

void main() {
    lan := Ethernet{jumbo: true}
    vpn := Tunnel{overhead: 80}
    describe(&vpn)
    List[dyn Link] path
    defer path.free()
    path.push(&lan)
    path.push(&vpn)
    i64 pmtu = 65535
    for l in path {
        pmtu = l.mtu() if l.mtu() < pmtu
    }
    print("{path.len} links, path MTU {pmtu}")
}
```

```output
tunnel carries 1420 bytes
2 links, path MTU 1420
```

A `dyn` value holds a pointer, not a copy: the struct must live as long as the `dyn` value is used. Its type must have each of the interface's methods with the same types. An interface with `Self` can't be used with `dyn`, since the type behind it isn't known when compiling.

## Which one to use

Bounds are the default: they cost nothing when the program runs, and the compiler checks each use with its own type. `dyn` is for values whose types are known only when the program runs, such as handlers of different kinds kept in one list, or plug-ins chosen by a setting. A call through `dyn` costs one lookup in the value's table.
