# if and match

`if` chooses by condition and `match` by value. Both can produce a value.

## if

`if` runs its block when the condition is true. `else if` adds conditions, tried in order. `else` runs when none matched.

```iris run
// How a ping's round trip reads, in milliseconds. 0 is no reply.
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

The condition is a `bool` without parentheses. Braces are required. `else` goes on the line of the closing brace or on the next line.

## Trailing conditions

A short statement can end with `if cond`. It runs only when the condition is true. Use it for guards at the top of a function and in loops:

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
        break if port == 0      // end of the list
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

- `stmt if cond` equals `if cond { stmt }`.
- It works after `return`, `break`, `continue`, an assignment, a call and `spawn`.
- With `else` after the condition it becomes a value (see below).
- Declarations and `defer` can't take one.
- `return if` followed by a block is an if-value: `return if ok { 1 } else { 2 }`.

A `void` function can put the condition after its `=>` body. The body runs only when the condition holds:

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

A function with a result can't, because it would have nothing to return. Use `a if cond else b` there.

## if as a value

Each branch ends with its value, without `return`. `else` is required.

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

A branch can run statements first. Its last line is the value.

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

All branches must have the same type.

`a if cond else b` is the one-line form of `if cond { a } else { b }`. It chains:

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

Without `else`, `return x if cond` is a guard.

## match

`match` runs the first arm that fits the value. Only that arm runs.

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

- An arm is its values, `=>`, and a statement or a block.
- `{}` does nothing.
- Several values are separated by commas.
- `_` matches everything the arms above didn't.

`match` works on integers, characters, `bool`, `str` and enums.

## Ranges

`low..high` in an arm matches both ends and everything between:

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

Each arm gives a value of one shared type. A function whose body is a single `match` uses `=>`, as `portKind` above does.

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

## Exhaustiveness

A `match` on an enum needs an arm for every value or a `_` arm. When a value is added to the enum, the compiler reports each `match` that misses it:

```iris error
enum Mode { read, write, append }

match m {
    .read => open()
    .write => create()
} // error: this match has no arm for Mode.append; add it or a '_' arm
```

A `match` value on integers or text needs a `_` arm.

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

Enum values are written `.read` because the type is known from the matched value. [Enums](15-enums.html) shows how `match` unpacks variants with data.
