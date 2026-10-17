# if and match

Choosing what runs: `if` for conditions, `match` for picking one of many values. Both can also give a value.

## if

`if` runs its block when its condition is true. `else if` adds more conditions, tried in order, and `else` runs when none was true:

```iris run
str describe(i64 n) {
    if n > 100 {
        return "big"
    } else if n > 10 {
        return "medium"
    } else if n > 0 {
        return "small"
    } else {
        return "nothing"
    }
}

void main() {
    for n in [500, 50, 5, 0] {
        print("{n}: {describe(n)}")
    }
}
```

```output
500: big
50: medium
5: small
0: nothing
```

The condition is a `bool` and needs no parentheses. The block always has braces. `else` goes on the line of the closing brace before it, or on the next line.

## A condition after a statement

A short statement can have its condition after it, on the same line: the statement runs only when the condition is true. This suits the checks at the top of a function and in loops:

```iris run
i64 sign(i64 n) {
    return -1 if n < 0
    return 0 if n == 0
    return 1
}

void main() {
    verbose := true
    print("{sign(-5)} {sign(0)} {sign(9)}")
    print("checking") if verbose
    for i in 0..10 {
        continue if i % 2 == 0
        break if i > 6
        print("odd {i}")
    }
}
```

```output
-1 0 1
checking
odd 1
odd 3
odd 5
```

`stmt if cond` is the same as `if cond { stmt }`. It works after `return`, `break`, `continue`, an assignment, a call or `spawn`, and takes only a condition. With an `else` after the condition it is a value instead (see below). A declaration can't have one, since the name would only exist when the condition held, and neither can `defer`.

`return if` followed by a block is still an if that gives a value: `return if ok { 1 } else { 2 }`.

## if as a value

`if` can give a value. Each branch ends with the value it gives, without `return`, and `else` is required, since there must be a value whichever way it goes:

```iris run
void main() {
    temperature := 31
    feel := if temperature > 30 { "hot" } else if temperature > 15 { "mild" } else { "cold" }
    print(feel)
}
```

```output
hot
```

A branch can hold statements before its value. The value is the last line of the branch:

```iris run
void main() {
    items := 7
    perPage := 3
    pages := if items > 0 {
        full := items / perPage
        full + (if items % perPage > 0 { 1 } else { 0 })
    } else {
        0
    }
    print("{pages} pages")
}
```

```output
3 pages
```

All the branches must give values of the same type.

A short choice of a value can be written on one line as `a if cond else b`: `a` when the condition is true, `b` when it isn't. It is the same as `if cond { a } else { b }`, and can be chained:

```iris run
str size(i64 n) => "small" if n < 10 else "medium" if n < 100 else "large"

bool even(i64 n) {
    return true if n % 2 == 0 else false
}

void main() {
    n := 42
    half := n / 2 if even(n) else n
    print("{size(n)} {half} {"even" if even(n) else "odd"}")
}
```

```output
medium 21 even
```

Without the `else`, `return x if cond` is a condition on the statement: it returns only when the condition holds.

## match

`match` compares one value against a list of arms and runs the first one that fits. Only that arm runs; there is no falling through into the next one:

```iris run
void run(str command) {
    match command {
        "start" => print("starting")
        "stop", "quit" => print("stopping")
        "" => {}
        _ => print("unknown command: {command}")
    }
}

void main() {
    run("start")
    run("quit")
    run("")
    run("dance")
}
```

```output
starting
stopping
unknown command: dance
```

An arm is the values it takes, `=>`, and what it does: one statement, or a block in braces. `{}` is an arm that does nothing. An arm can list several values, separated by commas. `_` takes every value the arms above it didn't.

`match` works on integers, characters, `bool`, `str` and enums.

## Ranges

An arm can be a range, `low..high`, which takes every value from `low` to `high` with both ends included:

```iris run
str grade(i64 score) => match score {
    90..100 => "A"
    80..89 => "B"
    70..79 => "C"
    _ => "F"
}

str kind(u8 c) => match c {
    'a'..'z', 'A'..'Z' => "letter"
    '0'..'9' => "digit"
    ' ', '\t', '\n' => "space"
    _ => "other"
}

void main() {
    print("{grade(95)} {grade(85)} {grade(42)}")
    print("{kind('q')} {kind('7')} {kind(' ')} {kind('#')}")
}
```

```output
A B F
letter digit space other
```

## match as a value

Like `if`, `match` can give a value. Each arm gives one, and they all have the same type. A function whose whole body is a `match` reads well with `=>`, as `grade` above shows:

```iris run
void main() {
    status := 404
    text := match status {
        200..299 => "ok"
        301, 302 => "moved"
        404 => "not found"
        _ => "error"
    }
    print("{status}: {text}")
}
```

```output
404: not found
```

## Every value has an arm

On an enum, a `match` must have an arm for every value, or a `_` arm. A missing value is a compile error, so adding a value to an enum shows every `match` that has to deal with it:

```iris error
enum Mode { read, write, append }

match m {
    .read => open()
    .write => create()
} // error: this match has no arm for Mode.append; add it or a '_' arm
```

On integers and text, where the values can't all be listed, a `match` that gives a value needs a `_` arm.

```iris run
enum Mode { read, write, append }

str flag(Mode m) => match m {
    .read => "r"
    .write => "w"
    .append => "a"
}

void main() {
    print("{flag(.read)}{flag(.append)}")
}
```

```output
ra
```

The enum's values are written with a dot, `.read`, since the type is known from the value being matched. The [Enums](15-enums.html) chapter shows how `match` takes apart enum values that carry data.
