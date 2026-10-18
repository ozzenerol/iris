# Tuples and type aliases

Tuples are the several values a function gives back. Type aliases give another name to a type. Both are small, and both make other code easier to read.

## Tuples

A tuple type lists types in parentheses: `(i64, str)`. A function returns one to give back several values, and the caller unpacks it into names right where the call is:

```iris run
// "db.lan:5432" is the host and the port.
(str, str) hostPort(str addr) {
    for i, c in addr {
        if c == ':' {
            return (addr[0:i], addr[i + 1:])
        }
    }
    return (addr, "")
}

void main() {
    host, port := hostPort("db.lan:5432")
    print("host {host}, port {port}")
}
```

```output
host db.lan, port 5432
```

The values go into new variables with `:=`, or into existing ones with `=`. `_` leaves one out. A tuple can't be kept whole in one variable; values that belong together beyond one call belong in a struct, whose fields have names:

```iris error
pair := hostPort("db.lan:5432") // error: this gives 2 values; unpack them into names: a, b := ...
```

Writing several values on the right of `:=` or `=` makes a tuple too, so two variables change in one step:

```iris run
void main() {
    seq, ack := 1000, 5000
    seq, ack = ack, seq + 1460
    print("{seq} {ack}")
}
```

```output
5000 2460
```

Every value on the right is worked out before any variable changes, so `seq + 1460` uses the old `seq`.

Tuples also give the values of a `for` loop over a type with a `next` method: `(T, bool)` for one value a round, `(K, V, bool)` for two (see [Loops](09-loops.html)).

## Type aliases

`type` gives a type another name. The two names mean the same type, so values of one can be used as the other:

```iris run
type Port = u16
type Handler = void(str)

void serve(Port port, Handler h) {
    h("listening on {port}")
}

void say(str s) => print(s)

void main() {
    Port p = 8080
    u16 same = p
    serve(p, say)
    print("{same}")
}
```

```output
listening on 8080
8080
```

An alias suits a type that is long to write, like a function type, or one that says what a value is for: `Port` reads better than `u16` in a signature. It is not a new type: a `Port` is a `u16`, with every `u16` operation.

A struct with one field is the way to get a type that can't be mixed up with another:

```iris run
struct ConnId {
    i64 v
}

struct StreamId {
    i64 v
}

void reset(StreamId id) => print("resetting stream {id.v}")

void main() {
    s := StreamId{v: 7}
    reset(s)
}
```

```output
resetting stream 7
```

Here `reset(ConnId{v: 7})` doesn't compile, while two aliases of `i64` would mix freely.
