# Tuples and type aliases

Tuples carry a function's several results. Type aliases give a type a second name.

## Tuples

A tuple type lists types in parentheses: `(i64, str)`. A function returns a tuple to return several values, and the caller unpacks it at the call:

```iris run
// Splits "db.lan:5432" into host and port.
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

Unpack into new variables with `:=` or existing ones with `=`. `_` discards a value. A tuple can't be stored whole. Values that stay together belong in a struct.

```iris error
pair := hostPort("db.lan:5432") // error: this gives 2 values; unpack them into names: a, b := ...
```

`.0`, `.1` and so on pick one value by position:

```iris run
(str, str) hostPort(str addr) {
    for i, c in addr {
        if c == ':' {
            return (addr[0:i], addr[i + 1:])
        }
    }
    return (addr, "")
}

void main() {
    print("the port is {hostPort("db.lan:5432").1}")

    kind := "text/html; charset=utf-8".cut(";").0
    print(kind)
}
```

```output
the port is 5432
text/html
```

A `for` loop unpacks a list of tuples with names in parentheses:

```iris run
void main() {
    for (name, port) in [("api", 8080), ("db", 5432)] {
        print("{name} listens on {port}")
    }

    for i, (name, port) in [("cache", 6379)] {
        print("{i}: {name}:{port}")
    }
}
```

```output
api listens on 8080
db listens on 5432
0: cache:6379
```

Several values on the right of `:=` or `=` form a tuple, so variables change in one step:

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

All values on the right are computed first, so `seq + 1460` uses the old `seq`.

A `next` method returns `(T, bool)` for a one-name loop or `(K, V, bool)` for two (see [Loops](09-loops.html)).

## Type aliases

`type` names an existing type. Both names mean the same type, so values mix freely.

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

Use an alias to shorten a long type, such as a function type, or to document intent: `Port` reads better than `u16` in a signature. A `Port` is still a `u16` with every `u16` operation.

For a distinct type that can't be mixed up, wrap the value in a one-field struct:

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

`reset(ConnId{v: 7})` doesn't compile. Two aliases of `i64` would mix freely.
