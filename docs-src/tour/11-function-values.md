# Functions as values

A function can be kept in a variable, passed to another function and stored in a struct. This chapter covers function types and what can be done with them.

## Function types

The type of a function is written as its return type followed by its parameter types in parentheses: `i64(i64, i64)` is a function that takes two `i64`s and gives one. A function's name, used without calling it, is a value of that type:

```iris run
// Two ways to pick which of n servers gets request number r.
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

A parameter can take a function, so a function can leave part of its work to the caller:

```iris run
// Calls attempt until it succeeds, at most times times.
bool retry(bool(i64) attempt, i64 times) {
    for i in 1..times + 1 {
        if attempt(i) {
            return true
        }
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

`List.sort` takes a function like this: one that says whether its first value goes before its second (see [Collections](22-collections.html)).

There are no unnamed functions: the function passed is declared like any other, with a name. Data it needs comes through its parameters, or through a struct it is a method of.

## Type aliases for functions

A function type is easier to read with a name. `type` gives one:

```iris run
// Takes a request body and gives the status of the reply.
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

A field can hold a function, which is called with the field's name like a method:

```iris run
// A command typed at a server's admin console.
struct Command {
    str name
    void(str) run
}

void stats(str name) => print("{name}: 12 connections open")
void reload(str name) => print("{name}: config read again")

void main() {
    Command[2] commands = [Command{name: "stats", run: stats}, Command{name: "reload", run: reload}]
    for c in commands {
        c.run(c.name)
    }
}
```

```output
stats: 12 connections open
reload: config read again
```

## null and comparing

A function value is `null` until it is set, and can be compared with `==` and `!=`, against `null` or another function. Calling a `null` one stops a debug build with a message:

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

## What carries over

A call through a function value passes its arguments by position. Defaults and names belong to the declaration, so they don't carry over to the value: every argument is given.

A function that can fail has a type that starts with `!`, as its declaration does: `!i64(str)`. A function that returns `void` can be given where one returning `!void` is wanted, since never failing is one way of not failing. This is how an HTTP route can take a plain handler or one that uses `try`:

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

A generic function, and a C function with `...`, can't be used as a value. Methods are called through their struct; a function value holds a plain function.
