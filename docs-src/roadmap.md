# Roadmap

Each step is a release. A release is made when everything in it works and the tests pass on Linux x86_64, Linux arm64 and macOS arm64.

| Version | Contents | Status |
|---|---|---|
| 0.1 | Lexer and parser for the whole syntax. `iris parse` prints the syntax tree. | released |
| 0.2 | Integers, `bool`, functions, default and named arguments, `if`, `for`, `match`, `const`, string literals, `print` and `panic`. The first programs that run. | released |
| 0.3 | Pointers, structs, methods, embedding, arrays, slices, `String`, `format`, `alloc`, `free` and type aliases. Use after free is caught in debug builds. | released |
| 0.4 | `!T`, `try`, `catch`, `defer`, `errdefer`, `scoped`, error traces, and plain enums for error codes. | released |
| 0.5 | Enums with data, flag enums, patterns that take values apart, and several return values. | released |
| 0.6 | Generic structs and functions, interfaces as bounds, and `dyn`. | released |
| 0.7 | Modules, projects, packages with `iris get`, and the first standard library modules: `io`, `fs`, `str` and `os`. | released |
| 0.8 | `import c`: C headers read with clang, C functions (also with `...`), structs, types and constants, and linking C libraries. | released |
| 0.9 | Floating point: `f32`, `f64`, the `math` module, `str.parseFloat`, and C's `float` and `double`. | released |
| 0.10 | Functions as values, callbacks from C, and `@export`. | next |
| 0.11 | Tasks, channels and the network library. | planned |

After 0.11, in no fixed order: `comptime` and `when`, attributes like `@json`, `iris fmt`, the language server, and a JIT for `iris run`.
