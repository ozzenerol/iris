# Tools

The `iris` command builds, runs and tests programs, and manages packages.

## Commands

| Command | What it does |
|---|---|
| `iris run main.iris` | compiles and runs a program |
| `iris build` | builds the project in the current folder |
| `iris test` | runs the tests |
| `iris fmt` | formats the source files |
| `iris get <package>` | adds a package to `iris.mod` |
| `iris doc` | shows the documentation from the comments |
| `iris parse file.iris` | prints the syntax tree |
| `iris lex file.iris` | prints the tokens |

## Tests

A `test` block can sit in any source file, next to the code it tests:

```iris
i64 add(i64 a, b) => a + b

test "add" {
    expect(add(2, 3) == 5)
    expect(add(-1, 1) == 0)
}
```

`iris test` runs every test block. Tests are left out of normal builds.

## Build modes

| Mode | Checks | Optimised |
|---|---|---|
| `debug` | all | no |
| `release-safe` | index checks | yes |
| `release-fast` | none | yes |

```shell
iris build --mode=release-safe
```

## Other platforms

`--target` builds for another platform:

```shell
iris build --target=aarch64-linux
iris build --target=x86_64-linux
iris build --target=aarch64-macos
```

## Debugging

Every build includes debug information, so `gdb` and `lldb` show Iris source lines and variable names.
