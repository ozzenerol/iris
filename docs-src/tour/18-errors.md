# Errors

A function that can fail says so in its type, and gives either its result or an error. The caller must do something with the error: pass it up with `try`, or handle it with `catch`. Nothing can be ignored by accident. This chapter covers making errors, passing them on, handling them, the trace a program prints when one reaches the end of `main`, and `panic` for bugs.

## A function that can fail

A return type written `!T` means the function gives a `T` or an error. `return error(code, message)` gives an error; any other `return` gives the value:

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

`!void` is a function that gives no value but can fail. Its successful end is a plain `return`, or the end of its block.

## Codes and messages

An error has a code and a message. The code is a value of a plain enum: it tells the program what went wrong, so code can decide what to do. The message is for people: it says what happened, with the values that matter. The message can be left out, `error(ParseError.empty)`, which gives an empty one.

Each module lists what can go wrong with it in an enum of its own: `fs.FsError`, `net.NetError`, `json.JsonError`. An error's code can be printed (`ParseError.empty`), compared, and matched on.

## try

`try` calls a function that can fail and, when it does, returns the same error from the function `try` is in. That function must be able to fail too:

```iris run
import str

// The number of bytes in a Range header's "from-to".
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

`try` is part of an expression, so it can sit where the value is needed: `return try f(x) + 1`, `print("{try load()}")`. Everything that was set up with `defer` and `scoped` still runs on the way out, so passing an error up never leaks (see [Memory](19-memory.html)). The error keeps its message and trace while that code runs, even when the code makes and catches errors of its own, as a rollback can.

## catch with a value

`catch` handles the error where the call is. A value after it is used when the call fails:

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

The value after `catch` is one value: a name, a literal, a call, or an expression in parentheses. The rest of the line applies to the result: `parse(s) catch 0 + 1` adds one to whichever value it gets.

## catch with a block

`catch e { ... }` runs the block when the call fails, with the error in `e`. When the call gives a value, the block's last line is the value to use instead; the block can also leave with `return`, `break` or `continue`:

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

For a function that gives no value, the block is just what to do; it can also be empty, to drop the error on purpose:

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

The error's message stays the same for the whole block, even when code in the block makes and handles errors of its own.

## Deciding by the code

`catch e match e.code` picks what to do by the error's code. Each arm gives a value or leaves:

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

## Passing an error on with more to say

A caught error can be returned as it is with `return e`, or turned into a new one with the same code and a longer message, which tells the reader what was being done:

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

`error(e.code)` makes a new error with another error's code.

## Errors can't be ignored

Calling a function that can fail without `try` or `catch` is a compile error. To drop an error on purpose, the call is assigned to `_`:

```iris error
save(data)     // error: the error from save is not handled; use try, catch or _ =
_ = save(data) // the error is dropped, and so is the value
```

## Errors in main

When `main` is `!void`, an error that reaches its end is printed with its code, its message, and the trace: where it was made, and each `try` or `return e` that passed it on. The program exits with status 1:

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

A message holds up to 1024 bytes, and a trace up to 64 steps.

## Bugs: panic

`!T` is for things that can go wrong in a correct program: a missing file, a closed connection, bad input. A bug is different: an index past the end, an integer that overflows, a `null` pointer used. The program stops at once, with a message and where it happened, and exits with status 2. `panic` does the same for a state the program should never be in:

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

A panic can't be caught. It is meant to be fixed in the code, not handled while the program runs.

## Tasks don't return errors

A function started with `spawn` or `thread` can't return `!T`, since nothing waits for its result: it handles its errors itself, or sends them on a channel to a task that does (see [Tasks and channels](24-tasks-and-channels.html)).
