# Tools

The `iris` command builds, runs, tests, formats and documents programs, gets packages and serves editors. This chapter covers each command, the build modes, building for other platforms, editors and debuggers.

## Commands

| Command | What it does |
|---|---|
| `iris run main.iris [args]` | compiles and runs a program; what follows the file goes to the program |
| `iris build main.iris` | compiles a program, or a project when given its folder; `-o name` names the output |
| `iris check main.iris` | checks a program and its tests without compiling it |
| `iris test [file] [name]` | runs the test blocks, or those whose name contains `name` |
| `iris test --build -o tests [file]` | writes the test program and does not run it; `./tests name` runs it |
| `iris bench [file] [name]` | runs the bench blocks, built with `release-fast` |
| `iris fmt file-or-folder` | puts code in the standard layout |
| `iris doc http` | shows the documentation of a module, a name, a file or a project |
| `iris mod init [path]` | makes `iris.mod`, which turns the folder into a project |
| `iris get pkg@version` | downloads a package and adds it to `iris.mod` |
| `iris lsp` | the language server, for editors |
| `iris gdb [args]` | runs `gdb` with the pretty printers for Iris values |
| `iris c header.h` | shows what `import c` makes of C headers |
| `iris ir main.iris` | prints the LLVM IR the compiler writes |
| `iris parse file.iris` | prints the syntax tree |
| `iris lex file.iris` | prints the tokens |
| `iris version` | prints the version |

`iris run`, `build`, `check`, `test`, `bench` and `ir` take these options:

| Option | What it does |
|---|---|
| `--mode=debug`, `release-safe`, `release-fast` | the build mode; `debug` when none is given |
| `--target=x86_64-linux` and the others | builds for another platform |
| `-l name` | links a C library |
| `-I folder` | a folder where `import c` looks for headers |
| `--cpu=native` or a CPU's name | release builds use what that CPU has, such as AVX2; `native` is the building machine's CPU. The program may not run on older CPUs |
| `--pgo-gen` | release builds: the program records where it spends its time, in the folder `iris-pgo` of wherever it runs |
| `--pgo=iris-pgo` | release builds: optimised with what `--pgo-gen` recorded (a folder, `.profraw` files or a `.profdata` file) |
| `--pgo` | `build` only: records a profile with the program's bench blocks and optimises with it, in one step |

`iris bench` also takes `--time=1s`, how long each bench's last round takes. For `run`, `test` and `bench`, options go before the file. `build`, `check` and `ir` take them before or after it.

## Build modes

| Mode | Checks | Optimised | For |
|---|---|---|---|
| `debug` | every one, and AddressSanitizer for memory | no | writing and testing the program |
| `release-safe` | indexes and slices, and division by zero | yes | most programs that run for real |
| `release-fast` | only indexes into arrays of a fixed size | yes | measured code that needs every bit of speed |

```shell
iris build --mode=release-safe main.iris
iris build main.iris -o server --mode=release-fast
```

A debug build is many times slower than a release build, so measure speed with a release one. A release build optimises the program and the runtime together, so the runtime's small functions are inlined where they are called. With a linker that can't do that, it builds without it.

A profile tells the optimiser which code runs most. `iris build --pgo` makes one and uses it in a single command:

```shell
iris build --pgo -o server main.iris
iris build --pgo --mode=release-safe -o server main.iris
```

- It builds the bench blocks with `--pgo-gen` and runs each for 200ms, which records the profile.
- A program with no bench blocks runs its test blocks instead. One with neither is an error.
- Then it builds the program with that profile.
- It builds with `release-fast` unless `release-safe` is given, and it takes no `--target`, since it runs the program on the machine that builds it.

Bench blocks that do the same work as the real program give the best profile. When the real work can't be put in a bench block, such as a server under load, record the profile by hand. Build with `--pgo-gen`, run the program on work like the real one, then build again with `--pgo=`:

```shell
iris build --mode=release-fast --pgo-gen -o server main.iris
./server            # writes iris-pgo/*.profraw as it runs
iris build --mode=release-fast --pgo=iris-pgo -o server main.iris
```

`--pgo` needs `llvm-profdata` of the same version as `clang`. `IRIS_PROFDATA` names another.

`iris` uses `clang`, version 15 or newer, to turn the IR into a program. `IRIS_CC` names another `clang`.

The runtime is the part of every program written in C. It is compiled once for each build mode and target and kept in `~/.cache/iris/rt` (or `rt` in `IRIS_PATH`). Later builds compile only the program's own code, so a small program builds in a tenth of a second. Builds with `--pgo-gen` or `--pgo` compile the runtime each time, and so does every build when `IRIS_NO_RT_CACHE` is set.

## Formatting

`iris fmt` puts code in the standard layout:

- four spaces for each level
- a space around operators and after commas, none inside brackets
- at most one blank line in a row
- end-of-line comments on neighbouring lines lined up in one column

It changes only spaces and line breaks. Comments and text in strings stay as they are, and a file whose tokens would change is left alone.

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

A comment at the top of a module's file, with a blank line after it, describes the module. For the standard library and packages, only `pub` names are shown. For a program's own files, everything is. The [Library](../std/index.html) pages are made from the same comments.

## Editors

`iris lsp` is a language server. An editor that speaks LSP gets:

- the errors as the code is typed
- the type and comment of a name under the pointer
- going to a name's declaration, also into the standard library
- the usages of a name in the project
- the rename of a name in the project
- a file's outline
- completion
- a quick fix that adds a missing import
- formatting with `iris fmt`

Completion offers the variables in scope at the cursor, the file's functions and types, the imported modules and the built-ins. After a dot it offers a module's names or a value's fields and methods, and in an `import` line the standard library's modules. A standard library module that the file doesn't import yet is offered too, by its name and after its dot, and taking it adds its `import` line. After `@` it offers the attributes of the declaration below: a struct's, a field's, an enum's or a function's. The server reads the editor's unsaved text and keeps answering while the line being typed doesn't parse yet.

The usages and the rename find the name in all files of the project: locals, parameters, fields, methods, functions, types, type parameters and enum members. A name in a module of the project also has its usages in the files that import the module. The rename doesn't change the names of the standard library or the built-ins. The new name must be a valid name and must not be a keyword.

Neovim 0.11 or newer needs the syntax files from the repository's `editors/vim` folder, which `build-and-install.sh` also copies to `/usr/local/share/iris/editors/vim`:

```output
vim.opt.rtp:append("/path/to/iris/editors/vim")
vim.lsp.config("iris", {
    cmd = { "iris", "lsp" },
    filetypes = { "iris" },
    root_markers = { "iris.mod", ".git" },
})
vim.lsp.enable("iris")
```

Vim uses the same syntax files with `set rtp+=/path/to/iris/editors/vim`.

The VS Code extension in `editors/vscode` highlights `.iris` files and the `iris` code blocks in Markdown, and starts `iris lsp`. `make vscode` packages it (it needs Node.js and npm):

```shell
make vscode
code --install-extension editors/vscode/iris.vsix
```

The `iris.path` setting picks the `iris` program when it isn't the one on the PATH.

The IntelliJ plugin in `editors/intellij` works in IntelliJ IDEA 2026.2 and the other JetBrains IDEs of that version. It highlights `.iris` files, indents with 4 spaces, starts `iris lsp`, and adds an Iris run configuration. A run button beside `main`, every `test` and every `bench` runs it, and `file.iris:LINE:COL` in the output opens that line. `make intellij` builds it against the installed IDE, and Settings, Plugins, Install Plugin from Disk installs `editors/intellij/iris-intellij.jar`.

Other editors start `iris lsp` as the language server for `.iris` files.

## Other platforms

`--target` builds for another platform:

```shell
iris build --target=aarch64-linux main.iris
iris build --target=x86_64-macos main.iris
```

The targets are `x86_64-linux`, `aarch64-linux`, `x86_64-macos` and `aarch64-macos`. `when` sees the target's `os` and `arch`, and `comptime` code still runs on the building machine.

On a Mac, both macOS targets work as they are. For another Linux, `clang` needs that platform's C library and a linker for it. `IRIS_SYSROOT` names a folder with the target's files, or they are installed where `clang` looks, such as Debian's `gcc-aarch64-linux-gnu` for `aarch64-linux`.

## Debugging

Every build has debug information, so `gdb` and `lldb` show Iris files, lines and names. Set a breakpoint by file and line, or by a function's name. Arguments, variables, structs, slices and enums show by name.

`iris gdb` runs `gdb` with the pretty printers for Iris values. It gives its arguments to `gdb`:

```output
$ iris build main.iris -o server
$ iris gdb ./server
(gdb) break handle
(gdb) run
Breakpoint 1, handle (req=..., res=...) at main.iris:42
(gdb) info locals
peer = "10.0.0.5"
names = List[str](len=2) = {"ann", "bob"}
hits = Map[str, i64](len=1) = {["/"] = 3}
f = Frame.Data = {stream = 1, size = 1200}
e = Parse.badPort
```

The printers show `str` and `String` as text, and `List`, `Map`, `Set`, `Deque` and slices with their items. An enum with data shows its value and fields, an `Error` shows its code, and a `!T` shows its value or its error.

A function's name in the debugger is its name, `handle`, or `Server.close` for a method. `break main.handle` and `break main.Server.close` also work, with the module first. In release builds the optimiser may put a function's code inside its callers, and the function then has no breakpoint of its own.

Each block, loop and match arm is a scope in the debug information, and a variable's scope starts at its line. `info locals` shows only the variables that the code at the stop can use. `watch total` and `watch p.x` stop the program when a variable or a field changes.

`break iris_panicked` stops the program when it panics, after the message is printed. `backtrace` then shows where the panic came from.

`info threads` names each thread after the function that `thread` started. `iris tasks` lists the tasks of every thread: the task's number, gdb's thread, its state, the function it runs, where it is in Iris code and the runtime call it waits in. A `*` marks the task of the selected thread:

```output
(gdb) iris tasks
  Id    Thread  State     Function             Where                        Waits in
* 1     1       running   main                 main.iris:46
  2     1       waiting   waiter               main.iris:24                 iris_chan_recv
  3     1       sleeping  sleeper              main.iris:29                 iris_sleep
  4     2       sleeping  worker               main.iris:34                 iris_sleep
  5     1       new       sleeper              -
```

The states are `running`, `ready` (it runs when the running task waits), `new` (spawned, not started yet), `sleeping` (in `sleep` or a timeout) and `waiting` (for a channel, a lock, the network or a file). `iris tasks --json` gives the same as JSON, for tools.

`iris task N` shows the stack of task N. `iris task N CMD` runs other commands on that stack, with `;` between them:

```output
(gdb) iris task 2 frame function waiter; info locals
#3  0x000055555567a62f in waiter (ch=0x507000000020) at main.iris:24
v = 0
```

A task that waits keeps its registers on its own stack. While the commands run, gdb's thread takes them, and gives its own back after. The program then goes on as before.

Each program also carries the printers in its `.debug_gdb_scripts` section. A plain `gdb` loads them when the program's folder is in its `auto-load safe-path`:

```shell
gdb -iex "add-auto-load-safe-path $PWD" ./server
```

`iris gdb --printers` prints the path of a copy of the printers' script, for a tool that starts `gdb` itself. The tool loads it with `-iex "source PATH"`.

The standard library and the runtime are inside the compiler. A build writes a copy of them to `~/.cache/iris/VERSION` (or `VERSION` in `IRIS_PATH`), with read-only files, so a debugger shows their source too.

In VS Code, the Iris extension debugs with gdb, with the pretty printers. Set a breakpoint in a `.iris` file and press F5. The extension builds the program and runs it under `gdb -i=dap` (gdb 14 or newer).

## The libraries Iris loads

Some modules use a C library that is loaded the first time a program uses it, so a program that doesn't use it needs nothing installed:

| Module | Library | Set to use another |
|---|---|---|
| TLS in `net` and `http` | OpenSSL 3 (`libssl`) | `IRIS_OPENSSL` |
| `sqlite` | SQLite 3 | `IRIS_SQLITE` |
| `compress`, and gzip in `http` | zlib | `IRIS_ZLIB` |

On macOS, OpenSSL comes from Homebrew (`brew install openssl@3`). SQLite and zlib come with the system.
