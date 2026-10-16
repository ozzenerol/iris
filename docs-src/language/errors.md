# Errors

A function that can fail returns its result or an error. The caller has to deal with the error, either by passing it up with `try` or by handling it with `catch`.

## Returning an error

A return type written `!T` means the function returns a `T` or an error. `return error(...)` returns an error, and a plain `return` returns the value:

```iris
enum ParseError { empty, notNumber, tooBig }

!i64 parsePort(str s) {
    if s.len == 0 {
        return error(ParseError.empty, "port is empty")
    }
    i64 n
    for c in s {
        if !isDigit(c) {
            return error(ParseError.notNumber, "'{s}' is not a number")
        }
        n = n * 10 + (c - '0')
    }
    if n > 65535 {
        return error(ParseError.tooBig, "{n} is above 65535")
    }
    return n
}
```

An error has a code and a message. The code is a value from an enum and tells the program what went wrong. The message is for people.

`!void` is a function that returns nothing but can fail.

A complete program:

```iris run
enum ParseError { empty, notNumber }

!i64 parseDigit(str s) {
    if s.len == 0 {
        return error(ParseError.empty, "nothing to parse")
    }
    c := s[0]
    if c < '0' || c > '9' {
        return error(ParseError.notNumber, "'{s}' is not a digit")
    }
    return i64(c - '0')
}

void main() {
    print("{parseDigit("7") catch 0}")
    n := parseDigit("x") catch e {
        print("{e.code}: {e.message}")
        -1
    }
    print("{n}")
}
```

```output
7
ParseError.notNumber: 'x' is not a digit
-1
```

## try

`try` calls the function and, if it returns an error, returns that error from the current function. The current function must also return `!`:

```iris
!Config loadConfig(str path) {
    scoped String text = try fs.readAll(path)
    Config c
    c.port = try parsePort(text.get("port"))
    return c
}
```

If `fs.readAll` fails, `loadConfig` returns the same error right away. `text` is still freed, because `scoped` and `defer` run on every way out.

## catch

`catch` handles the error where the call is. With a value after it, that value is used when the call fails:

```iris
port := parsePort(arg) catch 8080
```

With a name and a block, the block runs when the call fails, with the error in that name. The block either gives a value to use or leaves the function:

```iris
port := parsePort(arg) catch e {
    print("bad port: {e.message}, using 8080")
    8080
}

conf := loadConfig(path) catch e {
    print("can't start: {e.message}")
    return
}
```

## Deciding by the code

`e.code` can be checked with `match`:

```iris
conn := net.dial(host, 443) catch e match e.code {
    NetError.timeout => try net.dial(backup, 443)
    _ => return e
}
```

Here a timeout tries a second host, and any other error is returned to the caller.

## Adding context

An error can be returned again with a longer message:

```iris
user := db.findUser(id) catch e {
    return error(e.code, "loading user {id}: {e.message}")
}
```

## Errors can't be ignored

The result of a function that returns `!` must go through `try` or `catch`. Calling it and doing nothing is a compile error. To drop an error on purpose, assign it to `_`:

```iris
save("out.txt", data)          // compile error: the error is not handled
_ = save("out.txt", data)      // the error is dropped
```

## Errors in main

When `main` returns `!void`, an error that reaches the end of `main` is printed with the functions it passed through, and the program exits with status 1:

```iris
!void main() {
    cfg := try loadConfig("app.conf")
    try serve(cfg)
}
```

```output
error: fs.notFound: app.conf: no such file
  at fs.readAll    std/fs.iris:88
  at loadConfig    main.iris:12
  at main          main.iris:31
```

The first line is the error's code and message. Below it is the trace: where the error was made, then each `try` or `return e` that passed it on. The message holds up to 1024 bytes, and the trace up to 64 steps.

## Bugs and panic

`!T` is for things that can go wrong while the program is correct: a missing file, a closed connection, bad input. A bug, like an index past the end, stops the program right away with a trace. `panic("message")` does the same for a state the program should never reach.

A stopped program prints `panic:`, the message and where it happened, and exits with status 2:

```output
panic: integer overflow
  at main.iris:3:23 in double
```
