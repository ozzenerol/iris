# A first program

What every Iris program is made of: `main`, statements, blocks, comments, and the commands that build and run it.

## Hello

```iris run
void main() {
    print("hello, world")
}
```

```output
hello, world
```

Save the program as `hello.iris` and run it:

```shell
iris run hello.iris
```

`iris run` compiles the program and runs it straight away. `iris build` writes the program to a file instead, named after the source file, which then runs on its own:

```shell
iris build hello.iris
./hello
```

`print` writes its text and a newline to standard output. Values go into the text in braces:

```iris run
void main() {
    name := "Iris"
    year := 2026
    print("hello from {name}, in {year}")
    print("2 + 3 is {2 + 3}")
}
```

```output
hello from Iris, in 2026
2 + 3 is 5
```

## main

A program starts in `main`, which can be written in three ways:

```iris
void main() { }  // always succeeds: the exit status is 0
i32 main() { }   // returns the exit status itself
!void main() { } // can fail: an error ends the program with status 1
```

The third one lets `main` use `try`, which passes an error up instead of handling it. An error that leaves `main` is printed with the path it took through the program:

```iris run
import str

!void main() {
    n := try str.parseInt("42")
    print("n is {n}")
}
```

```output
n is 42
```

The [Errors](18-errors.html) chapter covers errors in full.

The arguments a program was started with come from the `os` module, and the `flags` module reads them into a struct (see [Attributes](27-attributes.html)):

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
    a := 6
    b := 7
    print("{a * b}")
}
```

```output
42
```

A long statement goes on to the next line when the line ends with an operator or a comma, or while a parenthesis or a bracket is still open:

```iris run
void main() {
    price := 120
    count := 3
    total := price * count +
        15 -
        20
    print("total {total}")
    words := ["one",
        "two", "three"]
    print("{words.len} words")
}
```

```output
total 355
3 words
```

## Blocks

A block is a list of statements in braces. `if`, `for`, functions and the other statements that hold code always take a block, even for one statement, and their conditions need no parentheses:

```iris run
void main() {
    n := 12
    if n > 10 {
        print("big")
    }
    for i in 0..3 {
        print("round {i}")
    }
}
```

```output
big
round 0
round 1
round 2
```

The opening brace goes on the same line as the statement it belongs to.

## Comments

`//` starts a comment that runs to the end of the line. `/* */` holds a comment that can span lines.

```iris run
void main() {
    // this line is a comment
    x := 1 // and so is the end of this one
    /* this comment
       spans two lines */
    print("{x}")
}
```

```output
1
```

A comment right above a declaration describes it. `iris doc` shows it, and so does an editor when the pointer rests on the name:

```iris
// The larger of a and b.
i64 max(i64 a, i64 b) => if a > b { a } else { b }
```

## Names

Names start with a letter or `_`, and go on with letters, digits and `_`. Iris code writes types in `UpperCamelCase` and everything else in `lowerCamelCase`: `HttpError`, `readLine`, `maxBody`. Constants may be written in capitals: `MAX_CONN`.

The keywords can't be used as names:

```output
if else for in match break continue return struct union enum interface type
dyn pub const comptime when import try catch defer errdefer scoped spawn
select chan thread threadlocal asm test true false null
```

Neither can the names of built-in types (`i64`, `str`, `String`...) and built-in functions (`print`, `format`, `error`, `panic`, `alloc`, `free`, `sleep`).

## Layout

`iris fmt` puts code in the standard layout, so every Iris program reads the same way: four spaces for each level, a space around operators and after commas, and none inside brackets.

```shell
iris fmt main.iris
```

The [Tools](31-tools.html) chapter covers it and the other commands.
