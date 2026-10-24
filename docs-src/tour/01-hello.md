# A first program

The parts of every Iris program: `main`, statements, blocks and comments, and the commands that build and run it.

## Hello

```iris run
void main() {
    print("hello, world")
}
```

```output
hello, world
```

Save it as `hello.iris` and run it:

```shell
iris run hello.iris
```

`iris run` compiles the program and runs it. `iris build` writes an executable named after the source file:

```shell
iris build hello.iris
./hello
```

`print` writes its text and a newline to standard output. Values go into the text in braces:

```iris run
void main() {
    host := "db.internal"
    port := 5432

    print("connecting to {host}:{port}")
    print("{port + 1} is the next port")
}
```

```output
connecting to db.internal:5432
5433 is the next port
```

## main

A program starts in `main`. It has one of three forms:

```iris
void main() { }  // exit status 0
i32 main() { }   // returns the exit status
!void main() { } // an error ends the program with status 1
```

The third form can use `try`, which passes an error up to the caller. An error that leaves `main` is printed with the path it took through the program:

```iris run
import str

!void main() {
    port := try str.parseInt("8080")
    print("listening on {port}")
}
```

```output
listening on 8080
```

[Errors](18-errors.html) covers errors in full.

The `os` module gives the program's arguments. The `flags` module reads them into a struct (see [Command-line flags](../library-tour/05-flags.html)).

```iris
import os

void main() {
    for i in 0..os.argc() {
        print("argument {i}: {os.arg(i)}")
    }
}
```

## Statements

A statement ends at the end of its line. There are no semicolons.

```iris run
void main() {
    packets := 3
    mtu := 1500
    print("{packets * mtu} bytes at most")
}
```

```output
4500 bytes at most
```

A statement continues on the next line when the line ends with an operator or a comma, or while a parenthesis or bracket is open:

```iris run
void main() {
    payload := 1200
    frames := 3

    total := payload * frames +
        14 * frames +
        20 * frames
    print("{total} bytes on the wire")

    hosts := ["10.0.0.1",
        "10.0.0.2", "10.0.0.3"]
    print("{hosts.len} hosts")
}
```

```output
3702 bytes on the wire
3 hosts
```

## Blocks

A block is a list of statements in braces. `if`, `for`, functions and every other statement that holds code take a block, even for a single statement. Conditions need no parentheses.

```iris run
void main() {
    open := 120

    if open > 100 {
        print("too many connections")
    }

    for attempt in 0..3 {
        print("connecting, attempt {attempt}")
    }
}
```

```output
too many connections
connecting, attempt 0
connecting, attempt 1
connecting, attempt 2
```

The opening brace goes on the same line as its statement.

## Comments

`//` starts a comment that runs to the end of the line. `/* */` holds a comment over several lines.

```iris run
void main() {
    // a whole line
    retries := 5 // the end of a line
    /* two
       lines */
    print("{retries}")
}
```

```output
5
```

A comment directly above a declaration documents it. `iris doc` shows it, and so does an editor when the pointer rests on the name.

```iris
// How long to wait before attempt n, doubling from 100ms.
Duration backoff(i64 n) => 100ms * (1 << n)
```

## Names

A name starts with a letter or `_` and continues with letters, digits and `_`. Types are written in `UpperCamelCase` and everything else in `lowerCamelCase`: `HttpError`, `readLine`, `maxBody`. Constants may use capitals: `MAX_CONN`.

These keywords are reserved:

```output
if else for in match break continue return struct union enum interface type
dyn pub const comptime when import try catch defer errdefer scoped spawn
select chan thread threadlocal asm test true false null
```

So are the names of built-in types (`i64`, `str`, `String`...) and built-in functions (`print`, `format`, `error`, `panic`, `alloc`, `free`, `sleep`).

## Layout

`iris fmt` rewrites code in the standard layout: four spaces per level, a space around operators and after commas, none inside brackets.

```shell
iris fmt main.iris
```

[Tools](30-tools.html) covers it and the other commands.
