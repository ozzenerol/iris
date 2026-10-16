# The compiler

How source code becomes a program, step by step.

## The steps

```output
source -> lexer -> parser -> checker -> lowering -> IR writer -> clang -> program
```

Each step reads the result of the one before it. No step looks back.

### Lexer

The lexer reads the source file and splits it into tokens: names, numbers, strings, operators and keywords. It also decides where statements end. A newline ends a statement unless the line clearly continues, for example when it ends with an operator or a comma, or inside parentheses.

### Parser

The parser reads the tokens and builds a syntax tree. Each function, statement and expression becomes a node. The parser only checks the shape of the code. It does not know about types. `iris parse file.iris` prints the tree.

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

### IR writer

The IR writer turns the lowered code into LLVM IR, as a text file (`.ll`). LLVM IR is the input language of LLVM. It describes the program as simple instructions on typed values.

### clang

`clang` optimises the IR, turns it into machine code for the target, and links the program with the runtime and any C libraries.

## Why the IR is written as text

Writing a text file needs no LLVM headers or libraries. Any machine with `clang` can build Iris programs, and when the output is wrong you can read the `.ll` file. Later, the IR writer can switch to the LLVM C API, for faster builds and a JIT. Only this last step changes.

## Source layout

| Path | Contents |
|---|---|
| `src/main.c` | The `iris` command: reads arguments and runs the steps. |
| `src/lexer.c` | The lexer. |
| `src/parser.c` | The parser and the syntax tree. |
| `src/check.c` | The checker. |
| `src/lower.c` | Lowering. |
| `src/ir.c` | The IR writer. |
| `runtime/` | The small part written in C: program start, error traces, the task scheduler. |
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
