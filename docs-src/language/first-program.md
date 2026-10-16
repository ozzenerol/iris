# A first program

The smallest Iris program, and the basic rules every program follows.

## Hello

```iris run
void main() {
    print("hello")
}
```

Save it as `hello.iris` and run it:

```shell
iris run hello.iris
```

```output
hello
```

Every program starts in `main`. `print` writes its text followed by a newline.

## main

`main` can be written three ways:

```iris
void main() { }        // always succeeds
i32 main() { }         // returns an exit status
!void main() { }       // can fail
```

When `!void main` returns an error, the program prints the error and exits with status 1. The [Errors](errors.html) page covers this.

## Statements

A statement ends at the end of its line. There are no semicolons.

```iris run
void main() {
    n := 2
    print("{n * 21}")
}
```

```output
42
```

A long expression can continue on the next line when the line ends with an operator or a comma, or inside parentheses or brackets:

```iris
total := price * count +
    shipping -
    discount

connect("example.com",
        port: 443)
```

Blocks always need braces, even for one statement. Conditions need no parentheses.

```iris
if n > 10 {
    print("big")
}
```

## Comments

`//` starts a comment that runs to the end of the line. `/* */` can span several lines. A comment right above a declaration documents it, and `iris doc` shows it.

```iris
// Returns the larger of a and b.
i64 max(i64 a, b) => if a > b { a } else { b }

/* This comment
   spans two lines. */
```
