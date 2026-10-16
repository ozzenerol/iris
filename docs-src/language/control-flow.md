# Control flow

Choosing what runs with `if` and `match`, and repeating it with `for`.

## if

```iris
if n > 100 {
    print("big")
} else if n > 10 {
    print("medium")
} else {
    print("small")
}
```

`else` can go on the same line as the closing brace or on the next line.

`if` can also give a value. Each branch ends with the value it gives, and `else` is required:

```iris
port := if arg.len > 0 { parsePort(arg) } else { 80 }
```

## match

`match` compares a value against a list of arms and runs the first one that fits. Only that arm runs. There is no fall-through and no `break`.

```iris
match cmd {
    "" => {}
    "exit", "quit" => quit = true
    "help" => {
        printHelp()
        printVersion()
    }
    _ => print("unknown command: {cmd}")
}
```

An arm can list several values separated by commas, or a range written as `low..high` (both ends included). `_` matches anything left.

`match` works on integers, `str`, and enums. On an enum, every value must have an arm, or there must be a `_`. A missing value is a compile error, so adding a value to an enum shows every `match` that needs updating.

`match` can give a value too:

```iris
kind := match status {
    200..299 => "ok"
    404 => "not found"
    _ => "error"
}
```

## for

`for` is the only loop. It has four forms.

Forever, until a `break` or `return`:

```iris
for {
    conn := try ln.accept()
    spawn handle(conn)
}
```

While a condition is true:

```iris
for n > 0 {
    n = n / 2
}
```

Over a range of numbers. `0..n` counts from 0 to n - 1:

```iris
for i in 0..10 {
    print("{i}")
}
```

Over the items of a slice or array, optionally with their index:

```iris
for line in lines {
    print(line)
}

for i, line in lines {
    print("{i + 1}: {line}")
}
```

Over a map, with its keys and values:

```iris
for name, age in ages {
    print("{name} is {age}")
}
```

## break and continue

`break` leaves the loop. `continue` skips to the next round.

```iris
for line in lines {
    if line.len == 0 { continue }
    if line == "end" { break }
    handle(line)
}
```

To leave an outer loop from an inner one, give the outer loop a label:

```iris
outer: for row in rows {
    for cell in row {
        if cell == 0 { break outer }
    }
}
```

## A complete example

```iris run
str kind(i64 status) => match status {
    200..299 => "ok"
    404 => "not found"
    _ => "error"
}

void main() {
    for code in 199..202 {
        print("{code}: {kind(code)}")
    }
    print("404: {kind(404)}")
}
```

```output
199: error
200: ok
201: ok
404: not found
```
