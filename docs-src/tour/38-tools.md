# Tools

The `iris` command builds, runs, tests, formats and documents programs, gets packages, and serves editors. This chapter covers each command, the build modes, building for other platforms, editors and debuggers.

## Commands

| Command | What it does |
|---|---|
| `iris run main.iris [args]` | compiles and runs a program; what follows the file goes to the program |
| `iris build main.iris` | compiles a program, or a project when given its folder; `-o name` names the output |
| `iris check main.iris` | checks a program and its tests without compiling it |
| `iris test [file] [name]` | runs the test blocks, or those whose name has `name` in it |
| `iris fmt file-or-folder` | puts code in the standard layout |
| `iris doc http` | shows the documentation of a module, a name, a file or a project |
| `iris mod init [path]` | makes `iris.mod`, which turns the folder into a project |
| `iris get pkg@version` | downloads a package and adds it to `iris.mod` |
| `iris lsp` | the language server, for editors |
| `iris c header.h` | shows what `import c` makes of C headers |
| `iris ir main.iris` | prints the LLVM IR the compiler writes |
| `iris parse file.iris` | prints the syntax tree |
| `iris lex file.iris` | prints the tokens |
| `iris version` | prints the version |

`iris run`, `build`, `check`, `test` and `ir` take these options:

| Option | What it does |
|---|---|
| `--mode=debug`, `release-safe`, `release-fast` | the build mode; `debug` is the one used without it |
| `--target=x86_64-linux` and the others | builds for another platform |
| `-l name` | links a C library |
| `-I folder` | a folder where `import c` looks for headers |

For `run` and `test`, options go before the file. `build`, `check` and `ir` take them before or after it.

## Build modes

| Mode | Checks | Optimised | For |
|---|---|---|---|
| `debug` | every one, and AddressSanitizer for memory | no | writing and testing the program |
| `release-safe` | indexes and slices, and division by zero | yes | most programs that run for real |
| `release-fast` | none | yes | code that has been measured and needs every bit of speed |

```shell
iris build --mode=release-safe main.iris
iris build main.iris -o server --mode=release-fast
```

A debug build is many times slower than a release build, so speed is measured with a release one.

`iris` uses `clang`, version 15 or newer, to turn the IR into a program. `IRIS_CC` names another `clang` to use.

## Formatting

`iris fmt` puts code in the standard layout: four spaces for each level, a space around operators and after commas, none inside brackets, at most one blank line in a row, and comments at the ends of lines that are next to each other lined up in one column. It changes only spaces and line breaks: comments and text in strings stay as they are, and a file whose tokens would change is left alone.

```shell
iris fmt main.iris       # rewrites the file
iris fmt .               # every .iris file in this folder and the folders inside it
iris fmt --check .       # lists the files that aren't in the layout; exit status 1 if any
iris fmt - < main.iris   # reads standard input and prints the result, for editors
```

## Documentation

`iris doc` shows what the comments say, in the terminal. Each declaration is shown as it is written, with the comment lines right above it:

```shell
iris doc http              # a module of the standard library
iris doc http.Router       # a struct and its methods
iris doc str.cut           # one function
iris doc List              # names every module has
iris doc main.iris         # a file, or a project's folder
iris doc main.iris User    # one name in it
```

A comment at the top of a module's file, with a blank line after it, describes the module. For the standard library and packages, only `pub` names are shown; for a program's own files, everything. The [Library](../std/index.html) pages are made from the same comments.

## Editors

`iris lsp` is a language server. An editor that speaks LSP gets the errors as the code is typed, the type and comment of a name when the pointer rests on it, going to a name's declaration (also into the standard library), a file's outline, completion, and formatting with `iris fmt`. Completion offers the variables in scope where the cursor is, the file's functions and types, the imported modules and the built-ins; after a dot, a module's names or a value's fields and methods; and in an `import` line, the standard library's modules. It reads the editor's unsaved text, and keeps answering while the line being typed doesn't parse yet.

In Neovim 0.11 or newer, with the syntax files from the repository's `editors/vim` folder, which `build-and-install.sh` also copies to `/usr/local/share/iris/editors/vim`:

```output
vim.opt.rtp:append("/path/to/iris/editors/vim")
vim.lsp.config("iris", {
    cmd = { "iris", "lsp" },
    filetypes = { "iris" },
    root_markers = { "iris.mod", ".git" },
})
vim.lsp.enable("iris")
```

Vim uses the same syntax files with `set rtp+=/path/to/iris/editors/vim`. Other editors start `iris lsp` as the language server for `.iris` files.

## Other platforms

`--target` builds for another platform:

```shell
iris build --target=aarch64-linux main.iris
iris build --target=x86_64-macos main.iris
```

The targets are `x86_64-linux`, `aarch64-linux`, `x86_64-macos` and `aarch64-macos`. `when` sees the target's `os` and `arch`, and `comptime` code still runs on the machine that builds.

On a Mac, both macOS targets work as they are. For another Linux, `clang` needs that platform's C library and a linker for it: `IRIS_SYSROOT` names a folder with the target's files, or they are installed where `clang` looks, such as Debian's `gcc-aarch64-linux-gnu` for `aarch64-linux`.

## Debugging

Every build has debug information, so `gdb` and `lldb` show Iris files, lines and names. A breakpoint is set by file and line, and arguments, variables, structs, slices and enums show by name:

```output
$ gdb ./server
(gdb) break main.iris:42
(gdb) run
Breakpoint 1, f.main.handle (req=..., res=...) at main.iris:42
(gdb) info locals
user = {id = 7, name = {ptr = "Ada", len = 3, cap = 16}}
```

A function's name in the debugger is its module and name, `f.main.handle`, or `m.main.Server.close` for a method. In release builds the optimiser may put a function's code inside its callers, and the function then has no breakpoint of its own.

## The libraries Iris loads

Some modules use a C library that is loaded the first time a program uses it, so a program that doesn't use it needs nothing installed:

| Module | Library | Set to use another |
|---|---|---|
| TLS in `net` and `http` | OpenSSL 3 (`libssl`) | `IRIS_OPENSSL` |
| `sqlite` | SQLite 3 | `IRIS_SQLITE` |
| `compress`, and gzip in `http` | zlib | `IRIS_ZLIB` |

On macOS, OpenSSL comes from Homebrew (`brew install openssl@3`); SQLite and zlib come with the system.
