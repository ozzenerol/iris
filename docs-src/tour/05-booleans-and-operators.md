# Booleans and operators

The `bool` type, the operators, and the order in which an expression is evaluated.

## bool

A `bool` is `true` or `false`. Comparisons produce one and conditions take one:

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

A condition must be a `bool`. `if count` doesn't compile. Write the comparison: `if count != 0`, `if p != null`, `if name.len > 0`.

## And, or, not

`&&` is true when both sides are true, `||` when at least one is, and `!` negates. The right side of `&&` and `||` runs only when needed:

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

So `conn != null && conn.open` is safe: the second part doesn't run when the first is false.

`and`, `or` and `not` are word forms. `and` and `or` equal `&&` and `||`. `not` applies to everything up to the next `and` or `or`, so `not a == b` means `!(a == b)`. `!` applies only to the value right after it.

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

Pick one style per project. `not` can't appear where a single value is expected, as in `a == not b`. Use `!` or parentheses there.

## Comparing

| Operator | True when |
|---|---|
| `a == b` | a equals b |
| `a != b` | a differs from b |
| `a < b`, `a <= b` | a is smaller, or smaller or equal |
| `a > b`, `a >= b` | a is larger, or larger or equal |

- Numbers compare by value.
- `str` and `String` compare by bytes, with `==` and `!=` only.
- Structs compare field by field with `==` and `!=`.
- Plain enums compare by value, pointers by address.

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

`is` means `==` and `is not` means `!=`. `a is b` on two structs compares fields, the same as `==`.

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

| Operators | Meaning |
|---|---|
| `+ - * / %` | arithmetic |
| `& \| ^ ~` | bitwise and, or, xor, not |
| `<< >>` | shifts |
| `== != < <= > >=`, `is`, `is not` | comparison |
| `&& \|\| !`, `and or not` | logic on `bool` |
| `-x` | negation |
| `x++ x-- ++x --x` | increment and decrement (see [Variables](02-variables.html)) |
| `&x`, `*p` | address of x, value at p (see [Pointers](17-pointers.html)) |
| `a..b` | a up to but excluding b in `for` and slicing; a to b inclusive in `match` |
| `ch <- v`, `<-ch` | channel send and receive (see [Tasks and channels](24-tasks-and-channels.html)) |
| `= += -= *= /= %= &= \|= ^= <<= >>=` | assignment, a statement |

## Precedence

From tightest to loosest:

1. `x++`, `x--`, then the prefix operators `-x`, `!x`, `~x`, `&x`, `*p`, `++x`, `--x`
2. `* / % << >> &`
3. `+ - | ^`
4. `== != < <= > >=`, `is`, `is not`
5. `not`
6. `&&`, `and`
7. `||`, `or`

Operators on one level group left to right. Parentheses override the order.

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

`&` binds tighter than `!=`, so `flags & 0x02 != 0` means `(flags & 0x02) != 0` and tests the SYN bit.

## Conditions as values

`if` and `match` produce values, so there is no `?:` operator (see [if and match](08-if-and-match.html)):

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
