# Attributes

An attribute starts with `@` and goes on the line before a declaration. Some make the compiler write code for a struct that reads it from environment variables, from command-line flags, from JSON or from binary messages. Others change how a function is compiled. This chapter covers each one except `@json`, `@entity`, `@wire` and `@flags`, which have their own chapters: [JSON](../library-tour/01-json.html), [Rows as structs](../library-tour/35-rows-as-structs.html), [Binary messages](../library-tour/21-binary-messages.html) and [Command-line flags](../library-tour/05-flags.html). A struct can have several, one to a line, such as `@json` and `@entity`.

| Attribute | On | What it does |
|---|---|---|
| `@json` | a struct | turns it into JSON and back, with the `json` module |
| `@entity` | a struct | makes it a row of a table, for the `sqlite`, `pg` and `mysql` modules; `@id` marks its key and `@column` names a column |
| `@wire` | a struct or an enum | reads it from binary messages and writes it, with the `wire` module |
| `@env` | a struct | fills it from environment variables, with the `env` module |
| `@flags` | a struct | fills it from the program's arguments, with the `flags` module |
| `@inline` | a function | always puts its code where it is called |
| `@unchecked` | a function | leaves out the checks of debug builds in its code |
| `@noreturn` | a function | says a call to it never comes back, like `panic` |
| `@export` | a function | makes it callable from C by its name |

## @env

`@env` lets `env.load` fill a struct from environment variables. Each field reads the variable named after it in capitals, with `_` between its words, so `dbUrl` reads `DB_URL`. A prefix on the struct goes in front of every name:

```iris run
import env

@env("PROXY")
struct Settings {
    u16 port = 8080                 // PROXY_PORT
    str upstream = "127.0.0.1:9000" // PROXY_UPSTREAM
    Duration timeout = 30s          // PROXY_TIMEOUT
    bool debug                      // PROXY_DEBUG
}

!void main() {
    scoped s := try env.load(Settings)
    print("{s.port} -> {s.upstream}, timeout {s.timeout}, debug {s.debug}")
}
```

```output
8080 -> 127.0.0.1:9000, timeout 30s, debug false
```

Run with `PROXY_PORT=80 PROXY_DEBUG=yes`, the same program prints `80 -> 127.0.0.1:9000, timeout 30s, debug true`. A variable that isn't set leaves the field's default.

`@env("NAME")` on a field gives the variable's whole name, without the prefix. `@env(required)` makes a missing variable an error. Both can go together:

```iris
@env
struct Secrets {
    @env("DATABASE_URL", required) String db
    @env(required) str apiKey // API_KEY
}
```

Fields can be:

- numbers
- `bool`: `true`, `false`, `1`, `0`, `yes`, `no`, `on`, `off`
- `str` and `String`
- plain enums, by their value's name
- `Duration`: `30s`, `1500ms`, `1m30s`

The errors are `env.EnvError.missing` and `invalid`, with the variable's name in the message:

```output
error: EnvError.missing: DATABASE_URL is needed but isn't set
```

An `@env` struct gets a `free` method that frees its `String`s, so `scoped` works on it.

`env.loadWith(Settings, path)` first reads a file of `KEY=value` lines into the environment and then fills the struct like `env.load`. A missing file is skipped, so the same program runs with only the environment:

```iris
import env

@env("PROXY")
struct Settings {
    str listenAddress = "0.0.0.0"
    u16 port = 8390
}

!void main() {
    scoped s := try env.loadWith(Settings, ".env")
    print("{s.listenAddress}:{s.port}")
}
```

A `.env` for it:

```shell
# the proxy
PROXY_LISTEN_ADDRESS="127.0.0.1"
PROXY_PORT=9001 # the port it listens on
```

- Blank lines and lines starting with `#` are skipped, and a `#` after a space starts a comment.
- A line may start with `export `.
- A value may be wrapped in double or single quotes, which are taken off. Inside them, `#` is part of the value.
- A variable that is already set keeps its value, so the environment wins over the file.
- A line with no `=`, a name that isn't letters, digits and `_`, or an unclosed quote is `EnvError.invalid`, with the file and line in the message.
- A file that exists but can't be read gives the error from [fs](../std/index.html#fs).

`env.loadFile(path)` does only the first half. It puts the file's lines into the environment, and a missing file is an error. It suits several files read in order before one `env.load`:

```iris
import env

@env("PROXY")
struct Settings {
    u16 port = 8390
}

!void main() {
    try env.loadFile(".env.local")
    try env.loadFile(".env")

    scoped s := try env.load(Settings)
    print("{s.port}")
}
```

The first file wins for a variable set in both, since a variable that is already set keeps its value.

## @flags

`@flags` lets `flags.parse` fill a struct from the program's arguments. Each field is a flag, and the help is made from the struct. The [Command-line flags](../library-tour/05-flags.html) chapter covers it.

## @inline

`@inline` puts the function's code at every place it is called, in place of a call. It suits a tiny function on a path that runs very often:

```iris run
@inline
u16 swap16(u16 x) => (x << 8) | (x >> 8)

void main() {
    print("{swap16(8080)}")
}
```

```output
36895
```

The optimiser already inlines small functions on its own in release builds. `@inline` makes sure of it.

## @unchecked

`@unchecked` leaves out the checks that debug builds add (indexes, overflow, `null`) from one function's code, in every build. It is for measured code whose indexes are known to be in range:

```iris run
@unchecked
u32 checksum(u8[] data) {
    u32 sum = 0
    for b in data {
        sum = sum * 31 + u32(b)
    }
    return sum
}

void main() {
    print("{checksum("GET / HTTP/1.1")}")
}
```

```output
2574418856
```

A mistake in such a function isn't caught. It reads or writes the wrong memory.

## @noreturn

`@noreturn` says that a call to the function never comes back. The function ends the program, as `os.exit` does, or panics. The compiler then knows the code after the call is never reached, so the call can end a `catch` block or a branch where a value would otherwise be needed:

```iris
import os, str

@noreturn
void fail(str msg) {
    print("error: {msg}")
    os.exit(2)
}

i64 port(str text) {
    n := str.parseInt(text) catch e {
        fail("the port {text}: {e.message}")
    }
    return n
}

void main() {
    print("{port("8080")}")
    print("{port("http")}")
}
```

```output
8080
error: the port http: 'http' is not a number
```

The program stops with exit status 2 at the second call.

A `@noreturn` function returns `void`, has no `return` and can't reach its end. Its last line is a call that never comes back, such as `os.exit`, `panic` or another `@noreturn` function.

## @export

`@export` makes a function callable from C by its name, for a library that calls back into Iris code. `@export("name")` gives it another C name. An exported function takes and gives only numbers and pointers. It can't return `!T`, since C has no way to take an error (see [Calling C](28-calling-c.html)):

```iris
@export
i32 onRow(u8 *ctx, i32 n, u8 **values, u8 **names) {
    return 0
}
```
