# Working on Iris

Where to start reading for a change, and how changes are built, tested and released. This applies to people and to agents.

## Where to start

Read `src/iris.h` first. Then follow the change through the pipeline:

- New syntax. Add the token to the `TOKENS` X-macro in `src/iris.h` and the keyword to `keyword` in `src/lexer.c` if there is one. Parse it in `src/parser.c`: `parseStmtCore` for a statement, `parsePrimary` or `parsePostfix` for an expression, `parseTopLevel` for a declaration. Give it a `NodeKind` if no existing one fits, print it in `printTree` in `src/ast.c`, and add a `tests/parse/` test with its `.tree` file.
- New rules or types. Check the node in `checkStmt` or `checkExprInner` in `src/check.c`. Put conversions in `coerce`, so every place that fits a value to a type agrees.
- New code generation. Write the node in `emitStmtInner` or `emitExpr` in `src/ir.c`. A call the compiler writes itself gets a `BI_` code in `src/iris.h`, set in `checkIntrinsic` or `checkCall`, and written in `emitBuiltin`.
- New runtime support. Add an `iris_` function to `runtime/rt.c`. Declare it in the list at the end of `writeIR` if the generated code calls it, or with `@extern("iris_name")` in a std file if the library calls it. Guard Linux-only code with `#ifdef __linux__`.
- New library code. Write it in Iris in `std/<module>/`. Every `pub` declaration needs a comment, which `iris doc` shows. Test blocks in the file run with the tests.
- New tooling. `iris fmt` is `src/fmt.c`, which works on tokens and has no settings. The language server is `src/lsp.c`, which checks the program in a child process for each request, so a compiler bug or hang loses one answer and not the server. `iris doc` is `src/doc.c`.

`iris lex`, `iris parse` and `iris ir` print each step's output and are the quickest way to see what the compiler did with a program.

## Adding a feature

1. Write the rules. The feature's chapter in the [Language Tour](../tour/index.html) says exactly how it behaves. If a rule is not settled, it goes to the maintainer as a question with a suggested answer.
2. Write the tests: programs that use the feature, and programs that must fail to compile.
3. Implement the feature.
4. Run all the tests on the three platforms.
5. Commit on a branch and merge it.

The maintainer decides the language design. Whoever writes the code decides how the compiler does it inside, and says what they chose.

## Building

```shell
make -j           # ./iris, one file per core
make iris-debug   # ./iris-debug, with AddressSanitizer and UBSan
make test         # all the tests
make test-debug   # all the tests with iris-debug
```

## Tests

`python3 tools/run_tests.py` runs every test. Names given after it pick the tests whose names contain them: `python3 tools/run_tests.py tour/24 errors/`. `--iris PATH` uses another compiler binary.

| Folder | What the test checks |
|---|---|
| `tests/parse/` | `iris parse` succeeds and prints the `.tree` file next to it. |
| `tests/errors/` | `iris check` fails with the errors listed at the top of the file. |
| `tests/run/` | `iris run` prints what the comments at the top say. |
| `tests/pg/`, `tests/redis/` | Like `run`, against a server that `tools/pg-cluster.sh` or `tools/redis-server.sh` starts. Skipped when none can start. |
| `tests/projects/` | A folder with `iris.mod`, run or checked as a whole. |
| `tests/fmt/` | `iris fmt` prints the `.out` file, and formatting that again changes nothing. |

The runner also checks that the standard library is formatted, runs the test blocks of each std file, drives an editor session against `iris lsp`, and tests `iris get` with a local git repository.

An error test lists each expected error with its line:

```iris error
// error: 4: this string has no closing quote

void main() {
    print("hello)
}
```

A run test says what the program prints. `// exit:`, `// panic:`, `// stderr:`, `// mode:`, `// command:`, `// args:`, `// env:`, `// stdin:` and `// tty:` set the rest of the run.

```iris
// expect: 7
void main() {
    print("{3 + 4}")
}
```

Every ```` ```iris ```` block in `docs-src/` must parse. A ```` ```iris run ```` block must also run and print the ```` ```output ```` block after it, where `…` matches anything. ```` ```iris error ```` blocks are skipped.

After changing the parser or `iris fmt` on purpose, `python3 tools/run_tests.py --update` rewrites the `.tree` and `.out` files. Check the diff before committing.

## Platforms

Every change is tested on Linux x86_64, Linux arm64 and macOS arm64. `tools/test-platforms.sh` builds Iris and runs the tests on all three at once, with the normal build and the AddressSanitizer build.

## Git

- `main` always builds and passes the tests.
- One branch per feature or bug: `feat/...` or `fix/...`.
- Commit messages are one line, lowercase, start with `feat:`, `fix:`, `docs:` or `test:`, and say what changed.
- Changes to the docs alone go straight to `main`.
- The repository is private for now.

## Releases

A release bumps `IRIS_VERSION` in `src/iris.h`. It happens when the work in it is done and the tests pass on all three platforms.

## Writing the docs

The docs are Markdown files in `docs-src/`. `python3 tools/build_docs.py` builds the HTML into `docs/` and fails on a broken link. Commit both.

Write plainly. Say what a thing does, in short sentences. Don't sell it, don't compare it with other languages, and give every feature an example.
