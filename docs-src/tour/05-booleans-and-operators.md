# Booleans and operators

The `bool` type, the operators that combine and compare values, and the order in which a longer expression is worked out.

## bool

A `bool` is `true` or `false`. Comparisons give one, and conditions take one:

```iris run
void main() {
    status := 503
    failed := status >= 500
    print("{failed} {status == 200} {status != 200}")
    if failed {
        print("retrying")
    }
}
```

```output
true false true
retrying
```

A condition must be a `bool`. A number, a pointer or a string isn't one, so `if count` doesn't compile; the comparison is written out: `if count != 0`, `if p != null`, `if name.len > 0`.

## And, or, not

`&&` is true when both sides are, `||` when at least one is, and `!` turns a `bool` around. The right side of `&&` and `||` is only worked out when it is needed:

```iris run
bool probe(str host, bool up) {
    print("probing {host}")
    return up
}

void main() {
    if probe("primary", false) && probe("replica", true) {
        print("both databases are up")
    }
    if probe("dns1", true) || probe("dns2", true) {
        print("a resolver is up")
    }
    print("{!true} {!(1 > 2)}")
}
```

```output
probing primary
probing dns1
a resolver is up
false true
```

This makes conditions like `conn != null && conn.open` safe: the second part isn't reached when the first is false.

They can also be written as words: `and`, `or` and `not`. `and` and `or` are the same as `&&` and `||`. `not` takes everything up to the next `and` or `or`, so `not a == b` is `!(a == b)`, where `!` takes only the value right after it:

```iris run
void main() {
    closed := false
    pending := 3
    if not closed and pending > 0 {
        print("flushing {pending} writes")
    }
    print("{not pending == 4} {closed or pending == 3}")
}
```

```output
flushing 3 writes
true true
```

Which to write is a matter of taste; a project usually picks one. `not` can't go where only one value fits, as in `a == not b`; parentheses or `!` are used there.

## Comparing

| Operator | True when |
|---|---|
| `a == b` | a and b are equal |
| `a != b` | they aren't |
| `a < b`, `a <= b` | a is smaller, or smaller or equal |
| `a > b`, `a >= b` | a is larger, or larger or equal |

Numbers compare by value. `str` and `String` compare by their bytes, `==` and `!=` only. Structs compare field by field with `==` and `!=`, plain enums by value, and pointers by address:

```iris run
struct Addr {
    u32 ip
    u16 port
}

enum Proto { tcp, udp }

void main() {
    a := Addr{ip: 0x7f000001, port: 8080}
    b := Addr{ip: 0x7f000001, port: 8080}
    print("{a == b} {"GET" == "GET"} {Proto.tcp != Proto.udp}")
    p := &a
    q := &b
    print("{p == q} {p == &a}")
}
```

```output
true true true
false true
```

`is` can be written for `==`, and `is not` for `!=`. They compare the same way, and the comparison is by value: `a is b` on two structs compares their fields, as `==` does, not whether they are the same variable.

```iris run
enum Proto { tcp, udp }

void main() {
    status := 404
    p := Proto.tcp
    print("{status is 404} {p is not .udp} {"GET" is "GET"}")
    print("not found") if status is 404
}
```

```output
true true true
not found
```

## All the operators

| Operators | What they do |
|---|---|
| `+ - * / %` | arithmetic |
| `& \| ^ ~` | bits: and, or, exclusive or, each bit flipped |
| `<< >>` | shifts |
| `== != < <= > >=`, `is`, `is not` | comparison |
| `&& \|\| !`, `and or not` | and, or, not, on `bool` |
| `-x` | the negative of x |
| `&x`, `*p` | the address of x, and the value p points to (see [Pointers](17-pointers.html)) |
| `a..b` | the range from a to before b, in `for` and slicing; in `match`, from a to b with both ends |
| `ch <- v`, `<-ch` | sending to a channel and taking from it (see [Tasks and channels](24-tasks-and-channels.html)) |
| `= += -= *= /= %= &= \|= ^= <<= >>=` | assignment, which is a statement |

## Order

In a longer expression, some operators bind more tightly than others. From tightest to loosest:

1. `-x`, `!x`, `~x`, `&x`, `*p` (the ones written before a value)
2. `* / % << >> &`
3. `+ - | ^`
4. `== != < <= > >=`, `is`, `is not`
5. `not`
6. `&&`, `and`
7. `||`, `or`

Operators on the same level go from left to right. Parentheses group anything differently:

```iris run
void main() {
    print("{2 + 3 * 4} {(2 + 3) * 4}")
    print("{10 - 4 - 3} {2 * 3 % 4}")
    flags := 0x12
    port := 8080
    print("{flags & 0x02 != 0} {port > 1023 && port < 49152 || port == 80}")
}
```

```output
14 20
3 2
true true
```

`flags & 0x02 != 0` reads as `(flags & 0x02) != 0`, since `&` binds more tightly than `!=`: it tests whether the SYN bit is set.

## Conditions as values

`if` and `match` give values too, which takes the place of the `?:` operator some languages have (see [if and match](08-if-and-match.html)):

```iris run
void main() {
    port := 443
    scheme := if port == 443 { "https" } else { "http" }
    print("{scheme}://example.com:{port}")

}
```

```output
https://example.com:443
```
