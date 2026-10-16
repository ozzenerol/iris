# Roadmap

Each step is a release. A release is made when everything in it works and the tests pass on Linux x86_64, Linux arm64 and macOS arm64.

| Version | Contents | Status |
|---|---|---|
| 0.1 | Lexer and parser for the whole syntax. `iris parse` prints the syntax tree. | next |
| 0.2 | Integers, `bool`, functions, `if`, `for`, `match`, `const`, string literals and `print`. The first programs that run. | planned |
| 0.3 | Pointers, structs, methods, arrays, slices, `str`, `String`, `alloc` and `free`. | planned |
| 0.4 | `!T`, `try`, `catch`, `defer`, `errdefer`, `scoped` and error traces. | planned |
| 0.5 | Enums with data, the rest of `match`, several return values and named arguments. | planned |
| 0.6 | Generics and interfaces. | planned |
| 0.7 | Modules, packages, `import c`, and the first parts of the standard library (`io`, `fs`, `str`). | planned |

After 0.7, in no fixed order: `comptime` and `when`, attributes, tasks and the network library, `iris fmt`, the language server, and a JIT for `iris run`.
