# Functions as values

Functions can be stored in variables and struct fields and passed as arguments.

## Function types

A function type is the return type followed by the parameter types in parentheses. `i64(i64, i64)` takes two `i64`s and returns one. A function's name without a call is a value of its type.

```iris run
// Two ways to pick which of n servers gets request r.
i64 roundRobin(i64 r, i64 n) => r % n
i64 firstOnly(i64 r, i64 n) => 0

void main() {
    i64(i64, i64) pick = roundRobin
    print("request 7 goes to server {pick(7, 3)}")

    pick = firstOnly
    print("request 7 goes to server {pick(7, 3)}")
}
```

```output
request 7 goes to server 1
request 7 goes to server 0
```

## Passing a function

A parameter can take a function, so the caller supplies part of the work:

```iris run
// Calls attempt until it succeeds, at most `times` times.
bool retry(bool(i64) attempt, i64 times) {
    for i in 1..times + 1 {
        return true if attempt(i)
    }
    return false
}

bool flaky(i64 n) {
    print("connecting, try {n}")
    return n == 3
}

bool dead(i64 n) => false

void each(str[] hosts, void(str) visit) {
    for h in hosts {
        visit(h)
    }
}

void ping(str host) => print("ping {host}")

void main() {
    print("{retry(flaky, 5)} {retry(dead, 2)}")
    each(["10.0.0.1", "10.0.0.2"], ping)
}
```

```output
connecting, try 1
connecting, try 2
connecting, try 3
true false
ping 10.0.0.1
ping 10.0.0.2
```

`List.sort` takes such a function: it returns whether its first argument goes before its second (see [Collections](22-collections.html)).

There are no anonymous functions. A passed function is declared with a name like any other. It gets data through its parameters or as a method of a struct.

## Type aliases

`type` names a function type:

```iris run
// Takes a request body and returns the reply status.
type Handler = i64(str)

i64 health(str body) => 200
i64 create(str body) => if body.len > 0 { 201 } else { 400 }
i64 gone(str body) => 410

void main() {
    Handler[3] routes = [health, create, gone]
    str[3] paths = ["GET /health", "POST /users", "GET /old"]

    for i, h in routes {
        print("{paths[i]}: {h("name=ada")}")
    }
}
```

```output
GET /health: 200
POST /users: 201
GET /old: 410
```

## In structs

A field can hold a function. It is called like a method, by the field's name:

```iris run
// A command typed at a server's admin console.
struct Command {
    str name
    void(str) run
}

void stats(str name) => print("{name}: 12 connections open")
void reload(str name) => print("{name}: config read again")

void main() {
    Command[2] commands = [
        Command{name: "stats", run: stats},
        Command{name: "reload", run: reload},
    ]

    for c in commands {
        c.run(c.name)
    }
}
```

```output
stats: 12 connections open
reload: config read again
```

## null and comparison

An unset function value is `null`. Function values compare with `==` and `!=`. Calling `null` stops a debug build.

```iris run
void logClose(str peer) => print("closed {peer}")

void main() {
    void(str) onClose
    print("{onClose == null}")

    onClose = logClose
    print("{onClose == null} {onClose == logClose}")

    onClose("10.0.0.5:51234")
}
```

```output
true
false true
closed 10.0.0.5:51234
```

## Calling through a value

- Arguments are positional. Defaults and parameter names belong to the declaration and don't carry over, so every argument is given.
- A function that can fail has a type starting with `!`: `!i64(str)`.
- A `void` function is accepted where `!void` is expected. HTTP routes use this to take plain handlers and handlers that use `try`.

```iris run
// A check run on each request, given its size in bytes.
type Check = !void(i64)

enum LimitError { tooBig }

!void limit(i64 size) {
    if size > 1MiB {
        return error(LimitError.tooBig, "{size} bytes is over the limit")
    }
    print("{size} bytes is fine")
}

void logSize(i64 size) => print("request of {size} bytes")

!void runAll(Check[] checks, i64 size) {
    for c in checks {
        try c(size)
    }
}

void main() {
    Check[2] checks = [logSize, limit]

    runAll(checks, 4096) catch e { }

    runAll(checks, 5MiB) catch e {
        print("rejected: {e.message}")
    }
}
```

```output
request of 4096 bytes
4096 bytes is fine
request of 5242880 bytes
rejected: 5242880 bytes is over the limit
```

Generic functions and variadic C functions can't be values. A function value holds a plain function. Methods are called through their struct.
