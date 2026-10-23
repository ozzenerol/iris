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
| 0.26 | `iris doc`, `@env` with the `env` module, `@inline` and `@unchecked`. | released |
| 0.27 | HTTP handlers that can fail, `res.error`, Router middleware and after hooks, `json.quote`, `sqlite.all` and `sqlite.one`, and the `log` module. | released |
| 0.28 | Timeouts on connections, server limits, `Router.shutdown`, and the `signal` module. | released |
| 0.29 | Generic methods checked when used (`List.contains`, `indexOf`, `freeEach`, `Map.keys`), and channels with no buffer. | released |
| 0.30 | WebSockets, `http.Client` with connections kept open, the `csv` module, `crypto.sha1`, and a guide to building a REST API. | released |
| 0.31 | Command-line flags from `@flags` structs, the `term` module (colors, tables, progress, line editing), `scoped name := value`, and a guide to building a command-line tool. | released |
| 0.32 | UDP, addresses, and a DNS resolver written in Iris that `net.dial` uses. | released |
| 0.33 | HTTP/2 for the server and the client, with HPACK, flow control and ALPN. | released |
| 0.34 | Regular expressions (`regex`), matched in linear time. | released |
| 0.35 | Compression: deflate and gzip, and gzip in HTTP. | released |
| 0.36 | A PostgreSQL driver written in Iris. | released |
| 0.37 | A Redis client, and structs that hold a `List` of themselves. | released |
| 0.38 | The documentation: the Language Tour, guides, and the library with a comment on every public name. Struct defaults everywhere, constant array sizes, integer limits, `json.decode` into slices. | released |
| 0.39 | `and`, `or` and `not` as words, a condition after a statement (`return -1 if n < 0`), any number of arguments with `T... name`, Tour chapters on the everyday library, log fields, `iris mod init`, and completion of local names in the language server. | released |
| 0.40 | `is` and `is not` for `==` and `!=`, and C names that are `not`, `and`, `or` or `is` imported with a `_` after them. | released |
| 0.41 | `@noreturn` (and `os.exit` can end a `catch` block), a clear error for a module that isn't imported, a fix for text built on one branch only, and go-to-definition on type names, module names, receivers and struct literal fields. | released |
| 0.42 | `str.format`, and constants made of text with constants in braces (`const Q = str.format("SELECT 1 FROM {TABLE}")`), worked out when compiling. | released |
| 0.43 | `a if cond else b`, a choice of value on one line (`return true if rows == 1 else false`). | released |
| 0.44 | The Language Tour's examples come from networking and systems work, a guide that builds a TCP chat server, `import c` follows a macro defined as another macro (`AF_INET` on Linux), and a generic used with `Duration` and with `i64` gets an instance for each. | released |
| 0.45 | `env.loadFile(path)` reads a file of `KEY=value` lines into the environment, so an `@env` struct can be filled from a `.env` file. | released |
| 0.46 | `env.loadWith(Settings, ".env")` reads a `.env` file when there is one and fills the `@env` struct in one call. | released |
| 0.47 | A `List` field of a `@json` struct is read and written as a JSON array, and `json.decode(List[T], text)` reads an array into a new list. | released |
| 0.48 | The functions of `str` are methods of `str` and `String` with no import: `path.startsWith("/api")`, `line.trim()`, `"8080".parseInt()`. The editor offers them after a dot. | released |
| 0.49 | `Conn.remoteAddr()` and `Conn.localAddr()` say who is on each side of a connection, and `req.remote` is the address an HTTP request came from. `fd()` gives a socket's file descriptor for socket options. | released |
| 0.50 | `n++`, `n--`, `++n` and `--n`, usable as values: `ports[count++] = 443`. | released |
| 0.50.1 | Go to definition and hover work on `Error`, `e.code`, `e.message`, `error`, `print` and the other built-in names. | released |
| 0.51 | `packed` structs, `align(n)`, bitfields (`u8 version: 4`), fields in network byte order (`u16be`, `u32be`, `u64be`), and `sizeof(T)`, so a protocol header is read straight from the bytes. | released |
| 0.51.1 | An unknown attribute says which ones the declaration takes, and `void *` says to use `u8 *`. | released |
| 0.52 | `Arena`: memory for many values that are freed together, with `a.alloc(T, n)`, `a.copy(text)`, `reset` and `free`. | released |
| 0.53 | `Set[T]`, which keeps each value once, with `add`, `has`, `remove`, `values` and loops over its values. | released |
| 0.54 | `crypto.sha512`, `hmacSha512`, `hkdfSha256`, and Ed25519 signatures with `ed25519Public`, `ed25519Sign` and `ed25519Verify`. | released |
| 0.54.1 | A program's output goes out whenever it waits, so a stopped server's output isn't lost when it goes to a file or a pipe. | released |
| 0.55 | `u64(p)` and `i64(p)` give a pointer's address, and `sizeof` and `alloc` take array types: `sizeof(Entry[4])`. | released |
| 0.56 | Methods with type parameters of their own, `Batch[T].map[U](U(T) f)`, with the types worked out from each call. | released |
| 0.57 | Encryption: X25519 key agreement with `x25519Public` and `x25519`, and ChaCha20-Poly1305 with `seal` and `unseal`. | released |
| 0.58 | A program's `.c` files are compiled and linked with it, and `asm` says to put assembly in one. | released |
| 0.59 | `ch.free()` frees a channel, and `scoped` works on channels. | released |
| 0.60 | The `dns` module reads and writes DNS messages, and the DNS server guide uses it. | released |
| 0.60.1 | A function that returns nothing can have a condition after its `=>` body: `void Dns.free() => self.cfg.free() if self.cfg is not null`. | released |
| 0.61 | `Set[String]` and `Map[String, V]` work. The set or map keeps its own copy of each `String`. | released |
| 0.61.1 | Several UDP sockets can listen on one port, such as one per thread, and the kernel gives each datagram to one of them. | released |
| 0.62 | `iris bench` and `bench` blocks. Batched UDP with `readMany` and `sendMany`, files mapped into memory with `fs.mapFile`, AES-GCM in `crypto`, `time.since`, and the new modules `pool`, `uuid`, `jwt` and `smtp`. | released |
| 0.63 | A VS Code extension: highlighting for `.iris` files and `iris` blocks in Markdown, and the language server. | released |
| 0.64 | Debugging in VS Code with gdb, the beagle icon for the extension and `.iris` files, and outline symbols VS Code accepts. | released |
| 0.65 | Faster programs: a memory allocator of its own, the runtime optimised with the program (LTO), `-O3` for `release-fast`, a Map that reads less memory, channels that stay fast with many waiting tasks, task stacks kept for reuse, small `alloc`s on the stack, and faster text building. `--cpu`, `--pgo-gen` and `--pgo` for release builds. Float constants can use arithmetic. An HTTP/2 server no longer leaks memory on each request and connection, and `iris run` leaves no files behind when it is killed. Benchmarks against Rust and Go in `bench/`. | released |
| 0.65.1 | Programs use less memory: memory fresh from the system is not written to until the program uses it, and big `String`s and `List`s keep less unused room. Short pieces of text are appended faster. | released |
| 0.66 | A larger standard library: `{value:spec}` formatting; UTF-8 runes, character classes and more text functions; checked, saturating and wrapping arithmetic; `big` numbers; `<` on text; the `sync` module (WaitGroup, Semaphore, Once, RwLock, Workers); Deque, Heap and SortedMap; sort, binarySearch, min and max on slices; `fs.walk`, `fs.find`, `fs.glob`, `fs.copy` and temporary files; more of `os` and `path`; `toml`, `ini`, `xml`, `tar`, `zip` and `template` modules; HTTP forms, uploads, cookies and files. Each new part is benchmarked against Rust and Go in `bench/`. | released |
| 0.67 | Networking the program sets up itself: `net.socket` takes options before it binds, listens or connects, a source address and a backlog; `setOption` and `option` reach any socket option, with `setNoDelay`, `setKeepAlive` and `setBuffers` for the common ones. `closeWrite` ends one direction, `net.copy` copies between connections (with `splice` on Linux), `peek` and `peekServerName` read the first bytes without taking them, and `received` and `sent` count bytes. TLS servers pick a certificate by name, `reload` their files, ask for client certificates and offer protocols with ALPN; clients show a certificate of their own. Unix sockets, passing descriptors with `sendFds` and `readFds`, `adoptListener` and `adoptConn`, and `waitRead` and `waitWrite` for descriptors used through `import c`. `net.listen` on every address takes IPv6 as well as IPv4. | released |
| 0.67.1 | Programs build faster: the runtime is compiled once per build mode and kept, so a small program builds in a tenth of a second. Changing a TLS server's settings keeps the certificates it loaded, and a TLS client needs the server's name or IP. The tests run several at a time. A Tour chapter on networking. | released |
| 0.68 | Eleven new Tour chapters on the library: regular expressions, configuration files, CSV and XML, encodings and ids, random numbers and cryptography, compression and archives, templates, PostgreSQL and Redis, the terminal, email, and math and big numbers, and the `sync` module in the threads chapter. An error keeps its message and trace while deferred code runs on the way out, even when that code makes errors of its own or waits. `res.html` and `res.send` answer with any `Content-Type`. | released |
