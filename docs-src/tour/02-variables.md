# Variables

How to declare variables, change them, and where each one is visible.

## Declaring with :=

`:=` declares a variable and gives it a value. The variable takes the value's type:

```iris run
void main() {
    port := 8080      // i64
    loss := 0.25      // f64
    tls := true       // bool
    host := "api.lan" // str
    timeout := 1500ms // Duration

    print("{host}:{port} tls={tls} loss={loss} timeout={timeout}")
}
```

```output
api.lan:8080 tls=true loss=0.25 timeout=1.5s
```

A whole number is an `i64` and a number with a point or an exponent is an `f64`, unless something else sets the type.

## Declaring with a type

Write the type before the name to pick a type other than the default:

```iris run
void main() {
    u8 ttl = 64
    i32 fd = -1
    f32 load = 1.5
    str method = "GET"

    print("{method} ttl={ttl} fd={fd} load={load}")
}
```

```output
GET ttl=64 fd=-1 load=1.5
```

The compiler checks that a number fits its type:

```iris error
u8 ttl = 300 // error: 300 doesn't fit in u8
```

## Zero values

A variable declared without a value starts at zero: `0` for numbers, `false` for `bool`, `""` for `str`, `null` for pointers. Structs and arrays have every field and item at zero.

```iris run
struct Endpoint {
    str host
    u16 port
}

void main() {
    i64 sent
    bool closed
    str peer
    f64 rtt
    i64[3] retries
    Endpoint e

    print("[{sent}] [{closed}] [{peer}] [{rtt}] [{retries[0]} {retries[2]}] [{e.host}:{e.port}]")
}
```

```output
[0] [false] [] [0] [0 0] [:0]
```

Struct fields with defaults start at their defaults (see [Structs](13-structs.html)). Every variable has a known value from the start.

## Assignment

`=` gives a variable a new value of the same type. Each arithmetic and bit operator has an assigning form:

```iris run
void main() {
    window := 4096
    window = 8192
    window += 1460 // 9652
    window -= 652  // 9000
    window *= 2    // 18000
    window /= 3    // 6000
    window %= 1460 // 160
    print("window {window}")

    flags := 0x02 // SYN
    flags <<= 3   // 16, the ACK bit
    flags |= 0x02 // 18, SYN and ACK
    print("flags {flags}")
}
```

```output
window 160
flags 18
```

Assignment is a statement. It has no value and can't appear inside an expression or a condition.

## Increment and decrement

`n++` adds one to `n` and `n--` subtracts one. As values, `n++` gives the old value and `++n` the new one. `n++` fills an array from the front:

```iris run
void main() {
    u16[4] ports
    count := 0

    ports[count++] = 22
    ports[count++] = 80
    ports[count++] = 443
    print("{ports[0]} {ports[1]} {ports[2]}, count {count}")

    retries := 3
    print("{retries--} then {retries}, {--retries} then {retries}")
}
```

```output
22 80 443, count 3
3 then 2, 1 then 1
```

They work on integers, floats and pointers (a pointer moves by one item), in variables, fields and array items, and check for overflow like `+=`. An assignment evaluates the right side first, then the target, so `a[i] = i++` stores the old `i` at the new index.

## Several at once

One statement can declare or assign several variables. All values are computed before any variable changes, so two variables swap without a temporary:

```iris run
void main() {
    src, dst := 49152, 443
    print("{src} -> {dst}")

    src, dst = dst, src
    print("{src} -> {dst}, the reply")

    host, port := "10.0.0.7", 22
    print("ssh {host} -p {port}")
}
```

```output
49152 -> 443
443 -> 49152, the reply
ssh 10.0.0.7 -p 22
```

The same form receives a function's several results (see [Functions](10-functions.html)). `_` discards a value:

```iris run
import str

void main() {
    user, _, found := str.cut("deploy@10.0.0.5", "@")
    print("{user} {found}")
}
```

```output
deploy true
```

## Scope

A variable is visible from its declaration to the end of its block, including nested blocks:

```iris run
void main() {
    payloads := [512, 1460, 88]
    total := 0

    for p in payloads {
        frame := p + 40 // only inside the loop
        total += frame
    }

    print("{total} bytes")
}
```

```output
2180 bytes
```

A name can't be declared again while it is visible, so a name means one thing throughout a function:

```iris error
x := 1
if ready {
    x := 2 // error: x is already declared on line 1
}
```

Sibling blocks can reuse a name, since neither sees the other:

```iris run
void main() {
    for i in 0..2 {
        port := 8080 + i
        print("web {port}")
    }

    for i in 0..1 {
        port := 9090
        print("metrics {port}")
    }
}
```

```output
web 8080
web 8081
metrics 9090
```

## Globals

A variable declared outside a function is a global. Every function in the module can use it, and it lives for the whole program:

```iris run
i64 requests
str server = "edge-1"

void handled() {
    requests += 1
    print("{server}: request {requests}")
}

void main() {
    handled()
    handled()
}
```

```output
edge-1: request 1
edge-1: request 2
```

A global's value must be known at compile time: a number, text, a constant, or an array or struct built from those. Setup that has to run goes at the start of `main` or into a `comptime` constant (see [Compile time](26-compile-time.html)). A global without a value starts at zero.

Top-level declarations can come in any order, so a function can use a global declared below it.

All tasks share globals. A global that several threads change needs an `Atomic` or a `Mutex`. `threadlocal` gives each thread its own copy (see [Threads](25-threads.html)).

## Constants

`const` names a value that never changes. It is computed at compile time from numbers, text and other constants:

```iris run
const KiB = 1024
const BUFFER = 64 * KiB
const HOST = "0.0.0.0"
const PORT = 8080
const LOSS_ALARM = 0.05

void main() {
    print("{HOST}:{PORT}, {BUFFER} byte buffers, alarm at {LOSS_ALARM} loss")
}
```

```output
0.0.0.0:8080, 65536 byte buffers, alarm at 0.05 loss
```

A constant has no address and takes no memory. Each use is the value itself. An integer constant takes the type its use needs if the value fits, so `const TTL = 64` works as a `u8` and as an `i64`.
