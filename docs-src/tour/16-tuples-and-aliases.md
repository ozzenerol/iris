# Tuples and type aliases

Tuples are the several values a function gives back. Type aliases give another name to a type. Both are small, and both make other code easier to read.

## Tuples

A tuple type lists types in parentheses: `(i64, str)`. A function returns one to give back several values, and the caller unpacks it into names right where the call is:

```iris run
(str, str) splitName(str full) {
    for i, c in full {
        if c == ' ' {
            return (full[0:i], full[i + 1:])
        }
    }
    return (full, "")
}

void main() {
    first, last := splitName("Ada Lovelace")
    print("{last}, {first}")
}
```

```output
Lovelace, Ada
```

The values go into new variables with `:=`, or into existing ones with `=`. `_` leaves one out. A tuple can't be kept whole in one variable; values that belong together beyond one call belong in a struct, whose fields have names:

```iris error
pair := splitName("Ada Lovelace") // error: this gives 2 values; unpack them into names: a, b := ...
```

Writing several values on the right of `:=` or `=` makes a tuple too, which is how two variables swap:

```iris run
void main() {
    a, b := 1, 2
    a, b = b + 10, a + 10
    print("{a} {b}")
}
```

```output
12 11
```

Every value on the right is worked out before any variable changes, so `b + 10` uses the old `b`.

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
struct UserId {
    i64 v
}

struct OrderId {
    i64 v
}

void cancel(OrderId id) => print("cancelling order {id.v}")

void main() {
    order := OrderId{v: 7}
    cancel(order)
}
```

```output
cancelling order 7
```

Here `cancel(UserId{v: 7})` doesn't compile, while two aliases of `i64` would mix freely.
