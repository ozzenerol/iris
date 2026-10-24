# Overview

What Iris is for, the path from source to program, and where each part lives in the source tree.

## What Iris is for

Iris is for programs that run close to the operating system: servers, proxies, databases, command-line tools and daemons. Programs should be short to write and fast to run. Iris is not designed for GUI applications, web front ends or data science.

Four ideas shape the design:

1. The language stays small. A feature is added only if it removes a lot of code and is easy to explain. This is why Iris has one loop keyword and no lambdas.
2. Costs are visible. A function that can fail says so in its return type (`!T`). One that hands back memory returns a type with a `free` method. There is no garbage collector, no hidden copy and no exception.
3. Iris works directly with C. It uses the C calling convention and lays out structs as C does. C headers can be imported directly, and C code can call Iris functions.
4. Debug builds check array bounds, integer overflow, null pointers and use after free. Release builds keep the bounds checks. A function marked `@unchecked` drops them.

## The pipeline

```output
iris build app.iris
  loadProgram     src/module.c    files -> Program (modules, parsed N_FILE trees)
    lexRange      src/lexer.c     text -> Token[]
    parseSource   src/parser.c    Token[] -> Node tree
  comptime        src/comptime.c  helper program, built and run, values read back
  checkProgram    src/check.c     Node tree -> same tree with Type, ref and IR flags filled in
  writeIR         src/ir.c        checked tree -> LLVM IR text
  compileIR       src/main.c      program.ll + runtime/rt.c -> clang -> executable
```

The driver is `src/main.c`. `cmdCompile` reads the options and calls `frontEnd`, which runs `loadProgram`, the comptime step when the program has one, and `checkProgram`. Then it calls `writeIR` and `compileIR`. Each step only reads the result of the step before it.

The subcommands stop at different points:

- `iris lex` and `iris parse` run one file through the lexer or the parser and print the result (`cmdLex`, `cmdParse`).
- `iris check` stops after `checkProgram`.
- `iris ir` prints the IR that `writeIR` returns.
- `iris build` writes the executable.
- `iris run` builds into a temporary folder, starts the program and removes the folder as soon as it has started.
- `iris test` and `iris bench` set `gTesting` or `gBenching`, which turn test and bench blocks into functions and give `main` a runner.

Everything the compiler allocates for one build comes from one `Arena` (`src/arena.c`) and is freed at once with `arenaFree`. Nodes, types and strings are never freed one by one.

## Source map

| Path | What it holds |
|---|---|
| `src/iris.h` | Every shared declaration: `Token`, `Node`, `Type`, `Module`, `Program`, the `N_`, `F_`, `TY_` and `BI_` enums. Read this first. |
| `src/main.c` | The `iris` command: `cmdCompile`, `frontEnd`, `runComptime`, `compileIR`, `iris mod`, `iris get`, `iris fmt`. |
| `src/arena.c` | `Arena` and `StrBuf`, the growing string every writer uses. |
| `src/diag.c` | `errorAt`, the error printer, and the hooks the language server sets. |
| `src/lexer.c` | `lexRange`: tokens and the newlines that end statements. |
| `src/parser.c` | `parseSource`: recursive descent into `Node` trees. |
| `src/ast.c` | `newNode`, `listPush` and `printTree`, which `iris parse` prints. It is the reference for what each node field means. |
| `src/module.c` | `loadProgram`: projects, imports, packages and the embedded standard library. |
| `src/cimport.c` | `cImport`: C headers to an Iris module, through clang. |
| `src/comptime.c` | The comptime helper's source and the JSON-to-literal reader. |
| `src/check.c` | `checkProgram`: names, types, layout, generics, `when`, tests, constants. |
| `src/generic.c` | `cloneNode`, the deep copy used for generic instances. |
| `src/ir.c` | `writeIR`: LLVM IR, debug information, JSON, `@env` and `@flags` tables. |
| `src/proc.c` | `runCommand`: runs clang, git and the built program. |
| `src/fmt.c` | `iris fmt`, which works on tokens. |
| `src/lsp.c` | `iris lsp`, the language server. |
| `src/doc.c` | `iris doc`. |
| `runtime/rt.c` | The C runtime: memory, errors, tasks, I/O, channels, threads, JSON, TLS, SQLite and the C side of many std modules. |
| `std/<module>/*.iris` | The standard library in Iris. `std/core` (`List`, `Map`, `Arena`...) is loaded into every program. |
| `tests/` | Test programs, run by `tools/run_tests.py`. |

The next pages follow the pipeline: [Loading and parsing](02-front-end.html), [The checker](03-checker.html), [The IR writer](04-ir.html), [How values are stored](05-values.html) and [The runtime](06-runtime.html). [Decisions](07-decisions.html) lists the language rules and why they were chosen, and [Working on Iris](08-workflow.html) says where to start for a change and how to test it.
