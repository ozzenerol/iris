# Loading and parsing

How the compiler finds the files of a program, turns them into tokens and builds the syntax tree.

## The loader

`loadProgram` in `src/module.c` returns a `Program` (`src/iris.h`): a linked list of `Module`s with the program's own module first, plus the C libraries and C files to link. Each `Module` has its import path, the name code uses for it, and its parsed files as `N_FILE` nodes.

1. `findRoot` looks for `iris.mod` from the program's folder upward. With one, the program is every `.iris` file of its folder (`readFolder`, sorted by name). Without one, the program is the one file.
2. `readModFile` reads `iris.mod`: `require` lines with package versions, `replace` lines that point a package at a folder, and `link` lines with C libraries.
3. The `core` module is loaded first. Its names are visible in every module without an import.
4. `loadImports` follows each `import` of the program's module. `loadModule` resolves an import in one of three ways:
   - A bare name (`import fs`) is a standard module. Its files are read from memory, from the `stdFiles` table.
   - A quoted path (`import "net/http"`) is a folder of the project, from its root.
   - A path that starts with a host name is a package. `packageDir` takes it from a `replace` folder or the package cache that `iris get` fills.
5. The `str` module is loaded last, because its functions are also methods of `str` and `String`. When the program didn't import it, `textImplicit` is set and `writeIR` compiles only the functions that are called.

Each module is parsed once. `Module.state` is 1 while a module's imports load, so meeting it again means a circle of imports, which is an error.

## The embedded standard library

The `iris` binary needs no other files. The Makefile turns every `std/*/*.iris` file into a byte array in `build/std.inc`, with a table `stdFiles[]` of `{path, data}`, and turns `runtime/rt.c` into `build/runtime.inc`. `module.c` includes the first and `main.c` the second. `stdSource(path)` gives the text of a std file to the language server and `iris doc`.

A std function reaches C in one of two ways:

- `@extern("iris_fs_read_all")` on a function without a body declares a C function from the runtime. The IR writer emits `declare` for it and calls it by that name.
- `@builtin` on a function without a body (`json.encode`, `flags.parse`, `Map`'s hashing) marks it `F_INTRINSIC`. The checker gives each call a `BI_...` code and the IR writer writes the code itself.

## import c

`loadCImports` collects the headers a file imports with `import c` and calls `cImport` in `src/cimport.c`. It runs `clang` twice: once with `-Xclang -ast-dump=json` for the declarations, and once with `-dM -E` for the macros. It writes what it finds as Iris source: `@extern` functions, structs, `type` aliases and integer constants. The loader then parses that text as a module named `c` with `Module.isC` set. Names it can't translate go into a `skipped` list, so the checker can say why `c.name` is missing. A C function with `...` gets `@extern("printf", variadic)`, and the IR writer declares it variadic.

## The lexer

`lexRange` in `src/lexer.c` splits text into an array of `Token`s, each with its kind (`TokKind`, from the `TOKENS` X-macro in `iris.h`), a pointer into the source, a length, a line and a column. The array ends with `T_EOF`.

The lexer also decides where statements end. At a line break, `lineBreak` adds a `T_NEWLINE` token when the last token can end a statement (`endsStatement`): a name, a literal, `)`, `]`, `}`, `++`, `--`, `return`, `break`, `continue`, `true`, `false` or `null`. Inside `( )` and `[ ]` line breaks add nothing. The parser never sees semicolons.

Text with values (`"got {n}"`) stays one string token. The parser splits it later and parses each value with `parseExprIn`, which runs the lexer again on that part of the source with its real line and column.

`iris lex file.iris` prints the tokens.

## The parser

`parseSource` in `src/parser.c` is a hand-written recursive descent parser. `parseTopLevel` reads declarations, `parseStmt` statements and `parseExpr` expressions. Binary operators use precedence climbing (`parseBinary` with `precedence`). The parser only checks the shape of the code and knows nothing about types.

Some statements can't be told apart by their first token. `List[i64] numbers` declares a variable, while `counts[key] = 1` assigns to an item. The parser calls `tryType`, and when a type is followed by a name and then `=`, a line end, `}` or the end of the file (`declFollows`), the statement is a declaration. Otherwise it goes back and reads an expression. A name followed by `{` starts a struct literal, except in the head of `if`, `for`, `match` and `when` (`parseHeadExpr`), where `{` opens the block.

On an error the parser reports it, skips to the end of the statement (`sync`) and goes on, so one run shows every syntax error in the file. The language server sets `gLenient`, which keeps the partial tree of a file with errors so names around the error stay known.

`iris parse file.iris` prints the tree with `printTree` from `src/ast.c`.

## The syntax tree

The whole tree uses one struct, `Node` in `src/iris.h`. `kind` is a `NodeKind`:

- Declarations: `N_FILE`, `N_IMPORT`, `N_FUNC`, `N_PARAM`, `N_TYPE_PARAM`, `N_STRUCT`, `N_FIELD`, `N_ENUM`, `N_INTERFACE`, `N_CONST`, `N_VAR`, `N_TEST`, `N_WHEN`...
- Types as written: `N_T_NAME`, `N_T_PTR`, `N_T_SLICE`, `N_T_ARRAY`, `N_T_GENERIC`, `N_T_FUNC`, `N_T_ERR` (`!T`), `N_T_DYN`, `N_T_CHAN`...
- Statements: `N_BLOCK`, `N_DEFINE` (`:=`), `N_ASSIGN`, `N_IF`, `N_FOR`, `N_MATCH`, `N_DEFER`, `N_ERRDEFER`, `N_SPAWN`, `N_SELECT`...
- Expressions: `N_IDENT`, `N_INT`, `N_STRING`, `N_INTERP`, `N_BINARY`, `N_CALL`, `N_INDEX`, `N_FIELD_ACCESS`, `N_STRUCT_LIT`, `N_TRY`, `N_CATCH`, `N_COMPTIME`, `N_THREAD`...
- Added by the checker: `N_CONVERT` for conversions it inserts, and `N_SELF_PARAM`.

The meaning of the general fields depends on the kind. `a`, `b`, `c` and `d` are child nodes, `list` and `list2` are lists of children, `attrs` holds attributes, and `name`, `text`, `ival`, `fval` and `op` hold names, decoded text, numbers and operators. For a function, `a` is the return type, `b` the receiver type of a method, `c` the body block or `d` a one-expression `=>` body, `list` the parameters and `list2` the type parameters. `ast.c`'s printer is the reference for every kind.

`flags` holds `F_` bits from the parser (`F_PUB`, `F_PACKED`, `F_POSTFIX`) and from the checker (`F_INSTANCE`, `F_TEMPTEXT`, `F_LAZY`...). The second half of `Node` is empty after parsing and filled in later: `type`, `ref` (the declaration a name refers to), `cval` for constants, `builtin`, `scope` and `module` by the checker, and `ir` (the LLVM name) by the IR writer.
