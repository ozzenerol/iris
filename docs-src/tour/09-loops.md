# Loops

`for` is the only loop, and it has four forms: forever, while a condition holds, over a range of numbers, and over the items of something. `break` and `continue` change its course.

## Forever

`for` with nothing after it runs until a `break` or a `return` leaves it:

```iris run
void main() {
    n := 1
    for {
        n *= 2
        if n > 100 {
            break
        }
    }
    print("{n}")
}
```

```output
128
```

A server's accept loop is the usual place for it: it runs as long as the program does.

## While a condition holds

`for` with a condition runs while the condition is true. It is checked before each round, so the loop may not run at all:

```iris run
void main() {
    n := 27
    steps := 0
    for n != 1 {
        n = if n % 2 == 0 { n / 2 } else { 3 * n + 1 }
        steps += 1
    }
    print("{steps} steps")
}
```

```output
111 steps
```

## Over a range

`for i in a..b` counts from `a` up to `b - 1`. The range's ends are worked out once, before the first round, so changing them in the loop doesn't change how many rounds there are. A range whose end isn't above its start runs no rounds:

```iris run
void main() {
    for i in 0..4 {
        print("round {i}")
    }
    for i in 3..3 {
        print("never")
    }
    total := 0
    for i in 1..101 {
        total += i
    }
    print("1 + ... + 100 = {total}")
}
```

```output
round 0
round 1
round 2
round 3
1 + ... + 100 = 5050
```

`i` is a new variable for each round, and it belongs to the loop. A range counts up by one; a loop that goes another way uses a condition:

```iris run
void main() {
    i := 10
    for i > 0 {
        print("{i}")
        i -= 4
    }
}
```

```output
10
6
2
```

## Over items

`for x in items` goes over the items of an array, a slice, a `str` (its bytes), a `List` or a `Map`. With two names, an array, a slice or a `str` gives each item's index first:

```iris run
void main() {
    i64[4] primes = [2, 3, 5, 7]
    for p in primes {
        print("{p}")
    }
    for i, p in primes[1:3] {
        print("{i}: {p}")
    }
    for i, b in "hi" {
        print("byte {i} is {b}")
    }
}
```

```output
2
3
5
7
0: 3
1: 5
byte 0 is 104
byte 1 is 105
```

A `List` gives its values. Its `items()` is a slice of them, which takes the form with an index:

```iris run
void main() {
    List[str] names
    defer names.free()
    names.push("ada")
    names.push("zoe")
    for n in names {
        print(n)
    }
    for i, n in names.items() {
        print("{i}: {n}")
    }
}
```

```output
ada
zoe
0: ada
1: zoe
```

A `Map` gives each key and its value, in no particular order. `_` leaves out what isn't needed:

```iris run
void main() {
    Map[str, i64] stock
    defer stock.free()
    stock.set("apples", 3)
    for name, count in stock {
        print("{name}: {count}")
    }
    for name, _ in stock {
        print("we have {name}")
    }
}
```

```output
apples: 3
we have apples
```

The items are copies: changing `x` in `for x in items` doesn't change the item. To change them, go over the indexes: `for i in 0..items.len { items[i] += 1 }`.

## break and continue

`break` leaves the loop at once. `continue` ends this round and goes on with the next:

```iris run
void main() {
    for n in 1..20 {
        if n % 2 == 0 {
            continue
        }
        if n > 9 {
            break
        }
        print("{n}")
    }
}
```

```output
1
3
5
7
9
```

## Labels

A label in front of a loop, `name:`, lets `break` and `continue` in a loop inside it act on the outer one:

```iris run
void main() {
    grid: for row in 0..3 {
        for col in 0..3 {
            if col > row {
                continue grid
            }
            if row == 2 && col == 1 {
                break grid
            }
            print("{row},{col}")
        }
    }
    print("done")
}
```

```output
0,0
1,0
1,1
2,0
done
```

## Making a type a loop can go over

Any struct can be gone over with `for`, by giving it a method `next(i64 *at)` that gives the next value and a `bool`, which is false when there are no more. `at` starts at 0 for each loop, and the method moves it on:

```iris run
struct Countdown {
    i64 from
}

(i64, bool) Countdown.next(i64 *at) {
    if *at >= self.from {
        return (0, false)
    }
    *at += 1
    return (self.from - *at + 1, true)
}

void main() {
    c := Countdown{from: 3}
    for n in c {
        print("{n}")
    }
    for n in (Countdown{from: 2}) {
        print("again {n}")
    }
}
```

```output
3
2
1
again 2
again 1
```

A method that gives two values and a `bool` makes a loop with two names, as a `Map` has. A struct literal in a `for` or `if` line goes in parentheses, so its braces aren't taken for the start of the block.
