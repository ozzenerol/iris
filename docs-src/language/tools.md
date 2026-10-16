# Tools

The `iris` command builds, runs and tests programs, and manages packages.

## Commands

| Command | What it does |
|---|---|
| `iris run main.iris` | compiles and runs a program |
| `iris build main.iris` | compiles a program, or a project when given its folder; `-o name` names the output |
| `iris check main.iris` | checks a program without compiling it |
| `iris ir main.iris` | prints the LLVM IR the compiler writes |
| `iris test` | runs the test blocks |
| `iris fmt` | puts source files in the standard layout |
| `iris get <package>@<version>` | downloads a package and adds it to `iris.mod` |
| `iris doc` | shows the documentation from the comments (planned) |
| `iris parse file.iris` | prints the syntax tree |
| `iris lex file.iris` | prints the tokens |
| `iris c header.h` | shows what `import c` makes of a C header |

## Tests

A `test` block can sit in any source file, next to the code it tests:

```iris
i64 add(i64 a, b) => a + b

test "add" {
    expect(add(2, 3) == 5)
    expect(add(-1, 1) == 0, "add(-1, 1) is {add(-1, 1)}")
}
```

`expect(cond)` checks a condition. When it is false, the test fails with the file and line, and the message if one is given, and goes on to the end. A test can use `try`: an error that leaves the test fails it, with the error's trace.

`iris test` runs every test block of the program's module, in the order they are written, and prints a line for each:

```output
$ iris test
ok   add
    main.iris:12:11: expect failed: n is 4
FAIL parse
2 tests, 1 passed, 1 failed
```

The exit status is 1 when a test failed. `iris test main.iris parse` runs only the tests whose name contains `parse`. `iris check` checks test blocks too; `iris run` and `iris build` leave them out.

## Formatting

`iris fmt` puts code in the standard layout: four spaces per level, spaces around operators and after commas, none inside brackets, at most one blank line in a row, and comments at the ends of lines next to each other in one column. Comments and the text in strings stay as they are.

```shell
iris fmt main.iris       # rewrites the file
iris fmt .               # every .iris file in the folder and the folders inside it
iris fmt --check .       # lists the files that aren't in the layout; exit status 1 if any
iris fmt - < main.iris   # reads standard input and prints the result, for editors
```

`iris fmt` only changes spacing and line breaks. As a check, it compares the code's tokens before and after, and leaves a file alone if they differ.

## Build modes

| Mode | Checks | Optimised |
|---|---|---|
| `debug` | all | no |
| `release-safe` | index checks | yes |
| `release-fast` | none | yes |

```shell
iris build --mode=release-safe main.iris
```

`iris run` and `iris build` use `clang`, version 15 or newer, to compile the IR. Set `IRIS_CC` to use a different one. `-l name` links a C library and `-I folder` adds a folder for C headers.

## Other platforms

`--target` builds for another platform:

```shell
iris build --target=aarch64-linux main.iris
iris build --target=x86_64-macos main.iris
```

The targets are `x86_64-linux`, `aarch64-linux`, `x86_64-macos` and `aarch64-macos`. `when` sees the target's `os` and `arch`.

On a Mac, both macOS targets work as they are, since Apple's tools have both. For another Linux, clang needs that platform's C library and a linker for it. Set `IRIS_SYSROOT` to a folder with the target's files, or install them where clang looks: on Debian, `gcc-aarch64-linux-gnu` for `aarch64-linux`.

## Debugging

Every build includes debug information, so `gdb` and `lldb` work with Iris source lines and names. A breakpoint is set by file and line, and arguments, variables, structs, slices and enums show by name:

```output
$ gdb ./server
(gdb) break main.iris:42
(gdb) run
Breakpoint 1, f.main.handle (req=..., res=...) at main.iris:42
(gdb) info locals
user = {id = 7, name = {ptr = "Ada", len = 3, cap = 16}}
```

A function's name in the debugger is its module and name: `f.main.handle`, or `m.main.Server.close` for a method. In release builds the optimiser may inline a function, which then has no breakpoint of its own.
