# Booleans and operators

The `bool` type, the operators that combine and compare values, and the order in which a longer expression is worked out.

## bool

A `bool` is `true` or `false`. Comparisons give one, and conditions take one:

```iris run
void main() {
    age := 20
    adult := age >= 18
    print("{adult} {age == 21} {age != 21}")
    if adult {
        print("can vote")
    }
}
```

```output
true false true
can vote
```

A condition must be a `bool`. A number, a pointer or a string isn't one, so `if count` doesn't compile; the comparison is written out: `if count != 0`, `if p != null`, `if name.len > 0`.

## And, or, not

`&&` is true when both sides are, `||` when at least one is, and `!` turns a `bool` around. The right side of `&&` and `||` is only worked out when it is needed:

```iris run
bool check(str what, bool value) {
    print("checking {what}")
    return value
}

void main() {
    if check("a", false) && check("b", true) {
        print("both")
    }
    if check("c", true) || check("d", true) {
        print("one")
    }
    print("{!true} {!(1 > 2)}")
}
```

```output
checking a
checking c
one
false true
```

This makes conditions like `p != null && p.len > 0` safe: the second part isn't reached when the first is false.

They can also be written as words: `and`, `or` and `not`. `and` and `or` are the same as `&&` and `||`. `not` takes everything up to the next `and` or `or`, so `not a == b` is `!(a == b)`, where `!` takes only the value right after it:

```iris run
void main() {
    done := false
    n := 3
    if not done and n > 0 {
        print("working")
    }
    print("{not n == 4} {done or n == 3}")
}
```

```output
working
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
struct Point {
    i64 x
    i64 y
}

enum Color { red, green }

void main() {
    a := Point{x: 1, y: 2}
    b := Point{x: 1, y: 2}
    print("{a == b} {"iris" == "iris"} {Color.red != Color.green}")
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
enum Color { red, green }

void main() {
    n := 3
    c := Color.red
    print("{n is 3} {c is not .green} {"iris" is "iris"}")
    print("small") if n is not 10
}
```

```output
true true true
small
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
    x := 6
    print("{x & 1 == 0} {x > 1 && x < 10 || x == 100}")
}
```

```output
14 20
3 2
true true
```

`x & 1 == 0` reads as `(x & 1) == 0`, since `&` binds more tightly than `==`: it tests whether x is even.

## Conditions as values

`if` and `match` give values too, which takes the place of the `?:` operator some languages have (see [if and match](08-if-and-match.html)):

```iris run
void main() {
    n := 7
    kind := if n % 2 == 0 { "even" } else { "odd" }
    print("{n} is {kind}")
}
```

```output
7 is odd
```
