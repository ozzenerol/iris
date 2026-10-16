# The compiler

How source code becomes a program, step by step.

## The steps

```output
source -> lexer -> parser -> checker -> lowering -> IR writer -> clang -> program
```

Each step reads the result of the one before it. No step looks back.

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

## Why the IR is written as text

Writing a text file needs no LLVM headers or libraries. Any machine with `clang` can build Iris programs, and when the output is wrong you can read the `.ll` file. Later, the IR writer can switch to the LLVM C API, for faster builds and a JIT. Only this last step changes.

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
| `src/ir.c` | The IR writer. |
| `runtime/rt.c` | The small part written in C: printing and panics now; error traces and the task scheduler later. |
| `std/` | The standard library, written in Iris. |
| `tests/` | Test programs. |

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
