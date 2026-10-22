# if and match

Choosing what runs: `if` for conditions, `match` for picking one of many values. Both can also give a value.

## if

`if` runs its block when its condition is true. `else if` adds more conditions, tried in order, and `else` runs when none was true:

```iris run
// How a ping's round trip time reads, in milliseconds; 0 is no reply.
str health(i64 ms) {
    if ms > 500 {
        return "very slow"
    } else if ms > 100 {
        return "slow"
    } else if ms > 0 {
        return "ok"
    } else {
        return "no reply"
    }
}

void main() {
    for ms in [800, 150, 12, 0] {
        print("{ms}ms: {health(ms)}")
    }
}
```

```output
800ms: very slow
150ms: slow
12ms: ok
0ms: no reply
```

The condition is a `bool` and needs no parentheses. The block always has braces. `else` goes on the line of the closing brace before it, or on the next line.

## A condition after a statement

A short statement can have its condition after it, on the same line: the statement runs only when the condition is true. This suits the checks at the top of a function and in loops:

```iris run
Duration retryDelay(i64 attempt) {
    return 0s if attempt == 0
    return 30s if attempt > 8
    return 100ms * (1 << attempt)
}

void main() {
    verbose := true
    print("{retryDelay(0)} {retryDelay(3)} {retryDelay(9)}")
    print("scanning") if verbose
    for port in [22, 80, 3000, 8080, 0, 9090] {
        break if port == 0      // the end of the list
        continue if port < 1024 // needs root
        print("trying {port}")
    }
}
```

```output
0s 800ms 30s
scanning
trying 3000
trying 8080
```

`stmt if cond` is the same as `if cond { stmt }`. It works after `return`, `break`, `continue`, an assignment, a call or `spawn`, and takes only a condition. With an `else` after the condition it is a value instead (see below). A declaration can't have one, since the name would only exist when the condition held, and neither can `defer`.

`return if` followed by a block is still an if that gives a value: `return if ok { 1 } else { 2 }`.

A function that returns nothing can put the condition after an `=>` body. The call runs only when the condition holds:

```iris run
struct Conn {
    i64 *buf
}

void Conn.show() => print("buf {*self.buf}") if self.buf is not null

void main() {
    i64 n = 42
    Conn a = Conn{buf: &n}
    Conn b = Conn{}
    a.show()
    b.show()
}
```

```output
buf 42
```

A function that returns a value can't, since it would have nothing to return when the condition is false. Write `a if cond else b` there.

## if as a value

`if` can give a value. Each branch ends with the value it gives, without `return`, and `else` is required, since there must be a value whichever way it goes:

```iris run
void main() {
    load := 0.93
    level := if load > 0.9 { "critical" } else if load > 0.7 { "high" } else { "normal" }
    print("load {load}: {level}")
}
```

```output
load 0.93: critical
```

A branch can hold statements before its value. The value is the last line of the branch:

```iris run
void main() {
    size := 4000
    mss := 1460
    segments := if size > 0 {
        full := size / mss
        full + (if size % mss > 0 { 1 } else { 0 })
    } else {
        0
    }
    print("{segments} segments")
}
```

```output
3 segments
```

All the branches must give values of the same type.

A short choice of a value can be written on one line as `a if cond else b`: `a` when the condition is true, `b` when it isn't. It is the same as `if cond { a } else { b }`, and can be chained:

```iris run
str sizeClass(i64 bytes) => "small" if bytes < 1KiB else "medium" if bytes < 1MiB else "large"

bool retryable(i64 status) {
    return true if status >= 500 else false
}

void main() {
    status := 503
    wait := 2s if retryable(status) else 0s
    print("{sizeClass(4096)} {wait} {"retry" if retryable(status) else "give up"}")
}
```

```output
medium 2s retry
```

Without the `else`, `return x if cond` is a condition on the statement: it returns only when the condition holds.

## match

`match` compares one value against a list of arms and runs the first one that fits. Only that arm runs; there is no falling through into the next one:

```iris run
void reply(str command) {
    match command {
        "PING" => print("+PONG")
        "QUIT", "EXIT" => print("+OK bye")
        "" => {}
        _ => print("-ERR unknown command '{command}'")
    }
}

void main() {
    reply("PING")
    reply("QUIT")
    reply("")
    reply("FLUSH")
}
```

```output
+PONG
+OK bye
-ERR unknown command 'FLUSH'
```

An arm is the values it takes, `=>`, and what it does: one statement, or a block in braces. `{}` is an arm that does nothing. An arm can list several values, separated by commas. `_` takes every value the arms above it didn't.

`match` works on integers, characters, `bool`, `str` and enums.

## Ranges

An arm can be a range, `low..high`, which takes every value from `low` to `high` with both ends included:

```iris run
str portKind(i64 port) => match port {
    0..1023 => "system"
    1024..49151 => "registered"
    49152..65535 => "dynamic"
    _ => "invalid"
}

str kind(u8 c) => match c {
    'a'..'z', 'A'..'Z' => "letter"
    '0'..'9' => "digit"
    ' ', '\t', '\n' => "space"
    _ => "other"
}

void main() {
    print("{portKind(22)} {portKind(8080)} {portKind(51000)} {portKind(70000)}")
    print("{kind('q')} {kind('7')} {kind(' ')} {kind('#')}")
}
```

```output
system registered dynamic invalid
letter digit space other
```

## match as a value

Like `if`, `match` can give a value. Each arm gives one, and they all have the same type. A function whose whole body is a `match` reads well with `=>`, as `portKind` above shows:

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
