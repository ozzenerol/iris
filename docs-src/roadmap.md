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
| 0.10 | Functions as values, callbacks from C, and `@export`. | released |
| 0.11 | Tasks, channels, `select`, `sleep`, and the `net` and `http` modules. | released |
| 0.12 | Text with values passed straight to `str` parameters, `http.serve` on every CPU core, and io_uring on Linux. | released |
| 0.13 | `thread`, channels between threads, `Atomic[T]`, `Mutex` and `threadlocal`. | released |
| 0.14 | `@json` with the `json` module, Strings from calls freed at the end of the statement, and `r"""..."""`. | released |
| 0.15 | Debug information: `gdb` and `lldb` show Iris lines and values, and AddressSanitizer reports show Iris lines. | released |
| 0.16 | `when`, `--target`, and fixes: `catch` takes one value, no copies of a `Mutex` or `Atomic`, and safer temporary text. | released |
| 0.17 | `iris fmt`, and `test` blocks with `expect` and `iris test`. | released |
| 0.18 | `iris lsp`, a language server, and syntax files for Vim and Neovim. | released |
| 0.19 | `List` and `Map`, `for` over structs with `next`, the `time` module, and Durations that print in units. | released |
| 0.20 | Folders, file information and files read a part at a time in `fs`, the `path` module, and `process.run`. | released |
| 0.21 | An HTTP `Router` and an HTTP client. | released |
| 0.22 | TLS: `https` for the HTTP client and server, and TLS connections in `net`. | released |
| 0.23 | `random`, `crypto` (SHA-256, HMAC), `base64`, `hex` and `url`. | released |
| 0.24 | The `sqlite` module. `[]` for an empty slice. | released |
| 0.25 | `comptime`. | released |

Next: `iris doc` with the remaining attributes, `@env`, `@inline` and `@unchecked` (0.26).
