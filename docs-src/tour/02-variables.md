# Variables

Declaring variables, giving them values, and where each one can be used.

## Declaring with :=

`:=` declares a new variable and gives it a value. The variable's type is the value's type:

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

A whole number with no other type to go by is an `i64`, and a number with a point or an exponent is an `f64`.

## Declaring with a type

A declaration can also name the type, followed by the variable and its value. This is the way to get a type other than the one the value would give:

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

A number given to a typed variable must fit in it. The compiler checks this:

```iris error
u8 ttl = 300 // error: 300 doesn't fit in u8
```

## Zero values

A variable declared without a value starts at zero: `0` for numbers, `false` for `bool`, `""` for `str`, `null` for pointers, and every field and item at zero for structs and arrays.

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

A struct whose fields have defaults starts with those defaults instead (see [Structs](13-structs.html)). Every variable starts with a known value, so reading one before giving it a value is never undefined.

## Assignment

`=` gives a variable a new value of the same type. The operators that change a variable in place are written with `=` after them:

```iris run
void main() {
    window := 4096
    window = 8192
    window += 1460  // 9652
    window -= 652   // 9000
    window *= 2     // 18000
    window /= 3     // 6000
    window %= 1460  // 160
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

Assignment is a statement: it gives no value, so it can't sit inside an expression or a condition.

## Several at once

Several variables can be declared or assigned in one statement, with the names on the left and the values on the right. Every value is worked out before any variable changes, so two variables can swap without a third:

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

The same form takes the several values a function returns (see [Functions](10-functions.html)). `_` stands for a value that isn't wanted:

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

A variable can be used from its declaration to the end of the block it is declared in. A block inside it can use it too:

```iris run
void main() {
    payloads := [512, 1460, 88]
    total := 0
    for p in payloads {
        frame := p + 40 // only inside the loop's block
        total += frame
    }
    print("{total} bytes")
}
```

```output
2180 bytes
```

A name can't be declared again while it is still visible, in the same block or in a block inside it. This keeps every name meaning one thing within a function:

```iris error
x := 1
if ready {
    x := 2 // error: x is already declared on line 1
}
```

Two blocks side by side can each have their own variable with the same name, since neither sees the other:

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

A variable declared outside any function is a global. Every function of the module can use it, and it lives as long as the program:

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

A global's value must be known when the program is compiled: a number, text, a constant, or an array or struct made of those. Work that has to run first goes at the start of `main`, or into a `comptime` constant (see [Compile time](26-compile-time.html)). A global with no value starts at zero.

Functions can use a global declared further down the file: declarations at the top level can come in any order.

Globals are shared by every task of the program. A global that several threads change at the same time needs an `Atomic` or a `Mutex`, and `threadlocal` gives each thread its own copy (see [Threads](25-threads.html)).

## Constants

`const` gives a name to a value that never changes. The value is worked out when compiling, from numbers, text and other constants:

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

A constant has no address and takes no memory: each use is the value itself. An integer constant takes the type the place it is used needs, as long as the value fits, so `const TTL = 64` can be given to a `u8` and to an `i64`.

