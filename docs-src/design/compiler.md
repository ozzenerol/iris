# The compiler

How source code becomes a program, step by step.

## The steps

```output
files -> loader -> lexer -> parser -> checker -> IR writer -> clang -> program
```

### Loader

The loader (`src/module.c`) finds the files to compile. It looks for `iris.mod` from the program's folder upward. With one, the program is every `.iris` file in its folder; without, the program is the one file. Then it follows the imports: the standard library's files are carried inside the `iris` binary, project modules are folders from the root, and packages come from `replace` folders or the package cache. Each module's files are parsed once; a circle of imports is an error.

`import c` (`src/cimport.c`) runs `clang` twice on the headers a file imports: once for their declarations, as JSON (`-Xclang -ast-dump=json`), and once for their macros (`-dM -E`). It writes what it finds as an Iris module, with `@extern` functions, structs, `type` aliases and constants, which the loader parses like any other. A function with C's `...` gets `@extern("printf", variadic)`, and the IR writer calls it as variadic.

Each step reads the result of the one before it. No step looks back.

The checker gives each module its own scope for names and each file one for its imports. Before checking, it rewrites `fs.readAll` into one qualified name, checking that `readAll` exists in `fs` and is `pub`. The IR writer puts the module in every name it writes (`@"f.fs.readAll"`), so modules can use the same names.

### Lexer

The lexer reads the source file and splits it into tokens: names, numbers, strings, operators and keywords. It also decides where statements end. A newline ends a statement when the last token on the line is a name, a literal, a closing `)`, `]` or `}`, or one of `return`, `break`, `continue`, `true`, `false` and `null`. Inside `( )` and `[ ]` newlines are ignored. `iris lex file.iris` prints the tokens.

### Parser

The parser reads the tokens and builds a syntax tree. Each function, statement and expression becomes a node. The parser only checks the shape of the code. It does not know about types. `iris parse file.iris` prints the tree.

Some code can't be sorted out by its shape alone. `List[i64] numbers` declares a variable, while `counts[key] = 1` assigns to an element. The parser first tries to read a type followed by a name. If that fails, it goes back and reads an expression. A name followed by `{` starts a struct literal, except in the head of `if`, `for`, `match` and `when`, where the `{` opens the block.

When the parser finds an error, it reports it, skips to the end of the statement and continues. One run shows every syntax error in the file.

### Checker

The checker walks the tree and works out the type of every expression. This is where names are looked up, function calls are matched with their functions, and generic functions get their types. Most compile errors come from here: an unknown name, a wrong type, an error that is not handled, an enum case missing from a `match`.

Type checking is a separate pass on purpose. When a compiler checks types while it parses, it often only knows half of a declaration when it needs all of it, which causes bugs that are hard to find.

### Lowering

Lowering rewrites the checked tree into a smaller language with only simple pieces: variables, assignments, branches, loops, calls and returns. The bigger features become these pieces here:

- `defer` and `errdefer` become copies of their code at every exit of the block.
- `try` becomes a check and an early return.
- `match` becomes comparisons and jumps.
- String interpolation becomes a series of writes.
- Each generic function becomes one plain function per set of types it is used with.

After lowering, the rest of the compiler never sees these features.

There is no separate lowering pass. The IR writer does this work itself: it handles `match`, `if` values and loops directly, and for `defer` it keeps a list of deferred statements for each block and writes them out at every way out of the block: the end, `return`, `break`, `continue` and a `try` that fails.

Generics are handled by the checker. A generic function or struct is never checked by itself. When the checker meets a use with new types, like `max(v1, v2)` with `Version` values, it copies the generic declaration's tree (`src/generic.c`), puts the types in place of the type parameters, and checks the copy like ordinary code. The copy is named after its types, `max[Version]`, and the IR writer compiles it like any other function. An error inside a copy is reported in the generic code, followed by a line that points at the use that made the copy.

### IR writer

The IR writer turns the lowered code into LLVM IR, as a text file (`.ll`). LLVM IR is the input language of LLVM. It describes the program as simple instructions on typed values.

### clang

`clang` optimises the IR, turns it into machine code for the target, and links the program with the runtime and any C libraries.

The runtime is `runtime/rt.c`. The build turns it into a C string inside the `iris` binary, so `iris` needs no other files. For each build, `iris` writes the IR and the runtime to a temporary folder, runs `clang` on both, and removes the folder.

## Checks in the generated code

In debug builds the IR writer adds a check before every operation that can go wrong: `+`, `-` and `*` that overflow (using LLVM's `with.overflow` intrinsics), division by zero, shifts by at least the width of the type, conversions that lose a value, and `null` pointers. Index and slice bounds are checked in debug and release-safe builds. A failed check calls the runtime, which prints `panic:`, the message and the place, and exits with status 2. Release-safe builds also keep the division checks. Release-fast builds have none, and arithmetic wraps around.

Debug builds also compile with AddressSanitizer: every function gets LLVM's `sanitize_address` attribute and `clang` links with `-fsanitize=address`. That catches use after free and other bad memory accesses. Leak reports are turned off for now.

## comptime

When a program has `const X = comptime expr`, the front end runs twice. First it adds a file to the program's module with a function, `irisComptimeMain`, that prints each comptime expression as JSON (`json.encode`) on a line of its own, checks that program with the comptime constants as zero-valued stand-ins, builds it for the machine it runs on and runs it. Then it loads the program again, and the checker replaces each comptime expression with the literal that the JSON describes: a constant for numbers, `bool`, text and enums, and a read-only global (an LLVM `constant`) for arrays and structs. `src/comptime.c` writes the helper's file and turns JSON into literals.

## Debug information

The IR writer adds DWARF metadata as it goes. The program gets a compile unit, each source file a `DIFile`, each function a `DISubprogram`, and every instruction a `!DILocation` with the line and column of the statement or expression it comes from. Each named variable and parameter gets a `DILocalVariable` and an `llvm.dbg.declare` on its stack slot, with a debug type made from its Iris type: basic types, pointers, structs with their fields, slices and `String` as structs, arrays and plain enums. All variables share the function's scope, which works because Iris doesn't allow a name to hide another in the same function. clang turns it into DWARF with `-g`, which every build passes.

## Why the IR is written as text

Writing a text file needs no LLVM headers or libraries. Any machine with `clang` can build Iris programs, and when the output is wrong you can read the `.ll` file.

## Source layout

| Path | Contents |
|---|---|
| `src/iris.h` | Declarations shared by all the files. |
| `src/main.c` | The `iris` command: reads arguments and runs the steps. |
| `src/arena.c` | Arenas and growing strings. |
| `src/diag.c` | Error messages. |
| `src/lexer.c` | The lexer. |
| `src/parser.c` | The parser. |
| `src/ast.c` | Syntax tree nodes and the printer for `iris parse`. |
| `src/check.c` | The checker. |
| `src/generic.c` | Copies of trees, for generic instances. |
| `src/module.c` | The loader: projects, imports, packages. |
| `src/cimport.c` | `import c`: C headers to an Iris module. |
| `src/proc.c` | Running clang and git. |
| `std/` | The standard library, in Iris. The build carries it inside `iris`, like the runtime. |
| `src/ir.c` | The IR writer, with the debug information and the JSON tables. |
| `src/fmt.c` | `iris fmt`. |
| `src/lsp.c` | `iris lsp`, the language server. |
| `src/comptime.c` | `comptime`: the helper program and its values. |
| `runtime/rt.c` | The part written in C: printing, panics, error traces, the task scheduler, threads, the network, JSON and tests. |
| `editors/vim/` | Syntax files for Vim and Neovim. |
| `tests/` | Test programs. `tools/run_tests.py` runs them. |

## Memory inside the compiler

The compiler allocates from arenas. Everything for one compile lives in one arena and is freed at the end in one step. Nodes, types and strings are never freed one by one.

## Compile errors

Every error gives the file, line and column, the source line, and a mark under the problem:

```output
main.iris:3:11: error: x is not declared
    print("{x}")
            ^
```

The compiler reports every error it can find in one run, then stops before writing any IR.
