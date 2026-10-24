# Errors

A function that can fail says so in its type and returns either its result or an error. The caller must pass the error up with `try` or handle it with `catch`. This chapter covers creating, propagating and handling errors, the trace printed when an error leaves `main`, and `panic` for bugs.

## Failing functions

A return type `!T` means the function returns a `T` or an error. `return error(code, message)` returns an error. Any other `return` returns the value.

```iris run
enum ParseError { empty, notNumber, tooBig }

!u16 parsePort(str text) {
    if text.len == 0 {
        return error(ParseError.empty, "no port given")
    }

    i64 n = 0
    for c in text {
        if c < '0' || c > '9' {
            return error(ParseError.notNumber, "'{text}' isn't a number")
        }

        n = n * 10 + i64(c - '0')
        if n > 65535 {
            return error(ParseError.tooBig, "{text} is above 65535")
        }
    }

    return u16(n)
}

void main() {
    for t in ["8080", "", "80a", "99999"] {
        p := parsePort(t) catch e {
            print("{e.code}: {e.message}")
            continue
        }
        print("port {p}")
    }
}
```

```output
port 8080
ParseError.empty: no port given
ParseError.notNumber: '80a' isn't a number
ParseError.tooBig: 99999 is above 65535
```

`!void` returns no value but can fail. It succeeds with a plain `return` or by reaching the end.

## Codes and messages

An error has a code and a message.

- The code is a plain enum value. Programs branch on it.
- The message is for people and says what happened, with the relevant values. `error(ParseError.empty)` leaves it empty.

Each module has its own error enum: `fs.FsError`, `net.NetError`, `json.JsonError`. Codes can be printed (`ParseError.empty`), compared and matched.

## try

`try` calls a failing function. On error, it returns the same error from the enclosing function, which must also be able to fail.

```iris run
import str

// The byte count of a Range header's "from-to".
!i64 rangeSize(str from, str to) {
    a := try str.parseInt(from)
    b := try str.parseInt(to)
    return b - a + 1
}

void main() {
    print("{rangeSize("0", "499") catch -1}")
    print("{rangeSize("500", "end") catch -1}")
}
```

```output
500
-1
```

`try` is an expression: `return try f(x) + 1`, `print("{try load()}")`. `defer` and `scoped` cleanup still runs on the way out, so propagating never leaks (see [Memory](19-memory.html)). The error keeps its message and trace while cleanup runs, even if cleanup creates and catches errors of its own, as a rollback can.

## catch with a value

A value after `catch` replaces the result when the call fails:

```iris run
import str

void main() {
    port := str.parseInt("abc") catch 8080
    print("{port}")
}
```

```output
8080
```

The fallback is one operand: a name, literal, call, or parenthesized expression. The rest of the line applies to the result, so `parse(s) catch 0 + 1` adds one to either value.

## catch with a block

`catch e { ... }` runs the block on failure with the error in `e`. For a call with a value, the block's last line is the replacement. The block can also leave with `return`, `break` or `continue`.

```iris run
import str

i64 portOr(str s, i64 fallback) {
    n := str.parseInt(s) catch e {
        print("can't read the port '{s}': {e.message}")
        fallback
    }
    return n
}

void main() {
    print("{portOr("8443", 443)}")
    print("{portOr("https", 443)}")
}
```

```output
8443
can't read the port 'https': 'https' is not a number
443
```

For a `!void` call the block is just the handler. An empty block drops the error deliberately.

```iris run
enum LogError { full }

!void flush(bool roomLeft) {
    if !roomLeft {
        return error(LogError.full, "the disk is full")
    }
    print("log flushed")
}

void main() {
    flush(true) catch e { }

    flush(false) catch e {
        print("log not flushed: {e.message}")
    }
}
```

```output
log flushed
log not flushed: the disk is full
```

`e.message` stays the same throughout the block, even if the block handles other errors.

## Branching on the code

`catch e match e.code` chooses by code. Each arm gives a value or leaves.

```iris run
enum FetchError { notFound, timeout, refused }

!str fetch(str what) {
    if what == "old" {
        return error(FetchError.notFound, "{what} is gone")
    }
    if what == "slow" {
        return error(FetchError.timeout, "{what} took too long")
    }
    return "the content"
}

str fetchOr(str what) {
    s := fetch(what) catch e match e.code {
        FetchError.notFound => "(nothing)"
        FetchError.timeout => "(try later)"
        _ => "(error)"
    }
    return s
}

void main() {
    print(fetchOr("old"))
    print(fetchOr("slow"))
}
```

```output
(nothing)
(try later)
```

## Adding context

`return e` passes a caught error on unchanged. `error(e.code, "...")` creates a new error with the same code and a message that says what was being done.

```iris run
import str

enum ConfigError { invalid }

!i64 readTimeout(str text) {
    n := str.parseInt(text) catch e {
        return error(e.code, "reading the timeout: {e.message}")
    }
    return n
}

!i64 readRetries(str text) {
    n := str.parseInt(text) catch e {
        return e
    }
    return n
}

void main() {
    _ = readTimeout("ten") catch e {
        print(e.message)
        0
    }

    _ = readRetries("") catch e {
        print("{e.code}")
        0
    }
}
```

```output
reading the timeout: 'ten' is not a number
ParseError.empty
```

## Errors can't be ignored

Calling a failing function without `try` or `catch` is a compile error. Assign to `_` to drop the error deliberately:

```iris error
save(data)     // error: the error from save is not handled; use try, catch or _ =
_ = save(data) // the error and the value are dropped
```

## Errors in main

When `main` is `!void`, an error that leaves it is printed with its code, message and trace: where it was created and each `try` or `return e` that passed it on. The exit status is 1.

```iris
import fs

!i64 configSize(str path) {
    scoped String text = try fs.readAll(path)
    return text.len
}

!void main() {
    n := try configSize("app.conf")
    print("{n} bytes")
}
```

```output
error: FsError.notFound: app.conf: no such file
  at fsError     std/fs/fs.iris:18
  at readAll     std/fs/fs.iris:30
  at configSize  main.iris:4
  at main        main.iris:9
```

A message holds up to 1024 bytes and a trace up to 64 steps.

## panic

`!T` covers failures in a correct program: a missing file, a closed connection, bad input. Bugs are different: an index past the end, an overflow, a `null` dereference. Those stop the program immediately with a message and location, exit status 2. `panic` does the same for states that should never happen:

```iris
void setLevel(i64 level) {
    if level < 0 || level > 3 {
        panic("level {level} is outside 0..3")
    }
}
```

```output
panic: level 7 is outside 0..3
  at main.iris:3:9 in setLevel
```

A panic can't be caught. Fix the code.

## Tasks

A function started with `spawn` or `thread` can't return `!T`, since nothing waits for its result. It handles its own errors or sends them over a channel (see [Tasks and channels](24-tasks-and-channels.html)).
