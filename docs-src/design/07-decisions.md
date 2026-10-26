# Decisions

The rules Iris follows and the reason for each, the ones still proposed, and the features Iris leaves out. Where a rule shapes the compiler or the runtime, the other Design pages show how.

## Settled

| Decision | Why |
|---|---|
| The compiler is written in C. | It builds in seconds with any C compiler and needs no libraries. |
| Declarations put the type first: `i64 n = 5`. `n := 5` takes the type from the value. | The type is the first thing a reader sees. `:=` keeps short code short. |
| A function that can fail returns `!T`. Errors are handled with `try`, `catch` and `errdefer`. | Every place that can fail is visible, and errors cost nothing when nothing fails. |
| No optional types and no `?` syntax. | One more idea to learn for little gain. |
| No lambdas. Callbacks are named functions. | Same reason. Without a garbage collector, capturing variables also needs rules. |
| No semicolons. Blocks need braces. Conditions need no parentheses. | Less to type, no dangling `else`. |
| `for` is the only loop. | `while` and `do` add nothing `for` can't do. |
| `match` replaces `switch`. `if` and `match` give values. | No fall-through bugs, and no need for a separate `?:` operator. |
| `and`, `or` and `not` can be written for `&&`, `\|\|` and `!`. `not` takes a whole comparison. | Conditions read as words. The symbols work too. |
| `is` and `is not` are `==` and `!=`: they compare values, not identity. | `if c is .red` reads as a sentence. Identity is a pointer comparison, `&a == &b`, so `is` has no other job. |
| `x in ...` and `x not in ...` look in a range, a list of values, a slice or an array, or call `has`. A range leaves out its end, as in `for`. | Tests like `port in 1..1024` and `method in ["GET", "HEAD"]` read as words, and `i in 0..n` means the same in a condition as in a loop. |
| A short statement can end with `if cond`: `return if n < 0`. No `else`, and not after a declaration or `defer`. | Guards at the top of a function take one line instead of three. |
| A last parameter `T... name` takes any number of arguments as a `T[]`. A slice is passed by name, `sum(xs: nums)`. | Typed, and no new idea: the function sees a slice, and the array for it is on the caller's stack. |
| No garbage collector. | Predictable memory use and latency. |

## Proposed

These are suggestions. They are used in the examples on these pages until they are settled.

| Proposal | Reason |
|---|---|
| No ownership checks for now. Memory is handled with `defer`, `scoped` and the debug-build checks. | A check that each value is freed once can be added later without breaking code. |
| Any pointer can be `null`. Debug builds check every use. | Non-null pointers would need their own syntax. |
| `print` writes its text and a newline. | The common case needs no `\n`. |
| Integer literals are `i64`. A number converts to a wider type on its own (`u8` to `i64`). Every other conversion is written out. | No surprises from silent truncation. |
| A module is a folder. Its files share their names. `pub` makes a name visible outside the module. | A module can be split into files freely, with no imports between them. |
| The message and trace of an error are kept in a per-thread buffer until the next error. | `!T` stays small and errors need no allocation. |
| Conditions must be `bool`. `if n` is an error, `if n != 0` is correct. | Catches mistakes like testing a count instead of comparing it. |
| A variable can't have the same name as another one in an enclosing block of the same function. | Shadowing hides bugs. |
| `self` in a method is a pointer to the value. | Methods can change the value without extra syntax. |
| A bug (index past the end, `panic`) stops the program. `!T` is only for failures a correct program can meet. | Keeps error handling for the cases that need it. |
| Operators bind in this order, tightest first: `* / % << >> &`, then `+ - | ^`, then comparisons, then `&&`, then `||`, then `..`. `x & 1 == 1` means `(x & 1) == 1`. | Bit tests read the way they are written. |
| `else` can go on the line after the closing brace. | Both styles work, so neither is a trap. |
| Arithmetic happens in the operands' type, with no promotion to a wider type. | The type of every result is visible, and overflow checks catch what a silent widening would hide. |
| A struct field's default value must be known when compiling. | The default is copied into every literal that leaves the field out. |
| `str` and `u8[]` are one type. | Text is bytes. One type needs one set of rules. |
| A `String` can be used where a `str` is expected. | Printing, comparing and slicing an owned string need no conversion. |
| `e.code` can be compared with a value of any enum. | Errors from different parts of a program use different enums. |
| An error that leaves `main` prints its code, message and trace, and exits with status 1. | Status 2 stays for panics. |
| Values of an enum with data can't be compared with `==`. | What equality means for the data is unclear; `match` says it exactly. |
| A function's several values are unpacked into names, or one is picked with `.0`, `.1`. A tuple can't be stored whole. | Values that stay together belong in a struct. |
| Only structs have methods, so only structs satisfy interfaces. | Integers stay plain values. A generic function over numbers needs no bound. |
| A generic struct's types are always written out: `List[str] names`. There are no static methods like `List[str].new()`. | One way to make a value: a declaration or a literal. |
| A generic function is checked for each set of types it is used with, not on its own. | Errors point at the generic code and at the use that caused them. |
| A folder with `iris.mod` is a project, and each of its folders is a module. A file outside a project is a program on its own. | Projects get modules. Small programs and scripts stay one file. |
| A package's version is a tag in its repository, and `iris get` downloads it with git. | No package server to run. |
| The standard library reaches C through `@extern("name")` on functions without a body, which only the standard library may use. Programs use `import c`. | Programs get one way in to C, which reads the real headers. |
| `.ptr` gives a slice's pointer, and `p[0:n]` slices a pointer without checks. | Needed to hand memory to and from C. |
| `import c` turns `void *` and function pointers into `u8 *`, and `char` into `u8`. | Text and memory are bytes in Iris. Callbacks come later. |
| A C name that is an Iris keyword or built-in name gets a `_`: `c.free_`. A struct named like a function becomes `c.struct_NAME`. | C keeps struct names apart from other names. Iris doesn't. |
| An integer constant can be used where a float is expected. An integer variable must be converted with `f64(...)`. | `x * 2` reads naturally. Mixing variables stays explicit, as with integers. |
| Floats print in the fewest digits that read back the same, as plain decimals between 1e-7 and 1e21. | Printing is exact without being noisy. |
| `u8 *` converts to and from any pointer on its own. | It is what C's `void *` becomes, and C hands those around. |
| Function values are named functions only: no lambdas and no closures. | No hidden allocation for captured variables. A context pointer does the job. |
| Each thread has its own event loop and its tasks never move to another thread. `http.serve` runs one loop per core. | A task needs no locks for its own data, and a server still uses every core. |
| `thread f(args)` starts a thread and gives a `Thread` to join, written like `spawn`. | One way to start work, whether it is a task or a thread. |
| A thread ends when its function and the tasks it started have finished. | `join` waits for all the work the thread was given. |
| Channels work between threads. | They are the simplest way to hand work from one thread to another. |
| `Atomic[T]` is a type used only through its methods, for integers and `bool`. All operations are sequentially consistent. | A plain read or write of it can't happen by mistake, and there are no memory orders to learn. |
| `Mutex` has `lock`, `unlock` and `tryLock`, used with `defer`. A task waiting for it lets the other tasks of its thread run. | Nothing new to learn, and a waiting task doesn't stop its thread. |
| `threadlocal` is for global variables only. | A local variable belongs to one task already. |
| `@json` works from a table the compiler writes for each type. The runtime encodes, decodes and frees from it. | One small piece of C serves every type, and the program stays small. |
| A JSON key is the field's name as written. A missing field keeps its default unless it is `@json(required)`. Unknown keys are skipped. | Matches what most APIs send, with one attribute for the strict case. |
| A `@json` struct without a `free` method gets one. | Decoded values work with `scoped`. |
| `@wire` describes the bytes on the wire and leaves the struct's memory layout alone. A field's length comes from a prefix or an earlier field, and one that can pass 65535 needs a `max`. | Fields stay addressable and aligned. A length from the network can't make a server allocate gigabytes. |
| The compiler writes the code that reads and writes each `@wire` struct, where `@json` has a table. | Binary messages are read at the speed of hand-written code: one check per run of fixed fields, then loads and byte swaps. |
| `wire.read` decodes straight from the connection's buffer, and starts again when the message turns out to be cut short. | The common case, a whole message in the buffer, is one pass. A short one says how many bytes it needs, so the retry waits for all of them. |
| A `String` from a call, passed straight to a `str` parameter, is freed when the statement ends. | `res.json(json.encode(u))` needs no variable and doesn't leak. Same rule as text with values. |
| `r"""..."""` is raw text over several lines. | JSON and other text with quotes and braces can be written as it is. |
| Every build has DWARF debug information: lines, functions, variables and types. | Debuggers and AddressSanitizer reports show Iris code. It costs nothing at run time. |
| `when` tests `os`, `arch` and `mode`. Only the chosen branch is checked. | Platform code can use names that exist only on that platform. |
| The value after `catch` is one operand: `f() catch 0 != 5` is `(f() catch 0) != 5`. | The fallback is almost always one value, and comparisons read as expected. |
| A call that gets temporary text or a temporary String can't return a `str`, slice or pointer. | The result could point into memory freed at the end of the statement. |
| `x++` and `x--` are values: the one from before the change; `++x` and `--x` give the one after. | `a[n++] = v` fills an array in one line. |
| Copying a `Mutex`, an `Atomic` or a struct that holds one is an error. | A copy would be a second lock that guards nothing. |
| `iris fmt` works on tokens and has no settings. Where spacing is ambiguous it keeps what was written. | One layout for everyone, and comments and strings can't be damaged. |
| `expect` reports a failure and the test goes on. An error from `try` ends the test. | One run shows every failed check of a test. |
| Test blocks are checked by `iris check` and `iris test`, and left out of programs. | Tests don't rot, and programs don't carry them. |
| The language server checks the program again for each request, in a child process. | A compiler bug or hang loses one answer, not the editor's server. Checking a program takes milliseconds. |
| `List` and `Map` are generic structs in a `core` module that every module sees. | They are ordinary Iris code, and no import is needed for something every program uses. |
| A `Map`'s keys are integers, `bool`, enums, pointers or `str`, and a `str` key is copied. | Keys can't dangle, and hashing and comparing need no interfaces. |
| `for` goes over any struct with `next(i64 *at)`. | One rule covers `List`, `Map` and the program's own types. |
| A `Duration` prints in units: `1.5s`. | Printed timings are readable without converting. |
| `process.run` takes the program and its arguments as one list, and a status other than 0 is no error. | No shell quoting problems. The program decides what its status means. |
| `readDir` gives the names sorted. | The same folder lists the same way on every system. |
| Router patterns use `:name` and `*name`. | Braces would be read as values in Iris text. |
| A String from a generic call is never freed on its own. | A container's getter gives the container's value, not a new one. |
| TLS uses OpenSSL, loaded with `dlopen` when a program first uses it. | No headers or link flags to build, and programs without TLS don't depend on OpenSSL. |
| TLS lives inside `net.Conn`. | Reading lines, the HTTP server and the client work the same over TLS. |
| Clients always check certificates and host names. There is no switch to turn that off. A test CA is given with `ca` or `SSL_CERT_FILE`. | Safe by default, with a way to trust a private CA. |
| `random`'s functions use a fast generator (xoshiro256**) per thread. Secrets come from `random.bytes`, the system's secure source. | Fast numbers for programs and safe bytes for keys, with the difference in the name. |
| SHA-256 and HMAC are in the runtime, in C, without OpenSSL. | Hashing works on every machine. |
| SQL arguments are a list of `sqlite.Value`, an enum with data. | Each value says its SQL type, and one call takes any mix. |
| `sqlite.scan` fills `@json` structs, by the names JSON uses. | One struct serves the API and the database. |
| `[]` where a slice is expected is an empty slice. | `exec(sql, [])` and defaults like `args = []` need no special case. |
| `comptime` builds a helper program from the same code, runs it, and reads the values back as JSON. | Comptime code is ordinary Iris with all of the library, with no interpreter to keep in step with the compiler. |
| An `@env` field reads the variable named after it in capitals with `_`, unless `@env("NAME")` says otherwise. | Most settings need no attribute at all. |
| `iris doc` shows declarations as they are written in the source. | Defaults and attributes read exactly as in the code, and nothing has to be checked first. |
| A function that returns `void` can be given where a function that returns `!void` is wanted. | HTTP handlers can use `try`, and the ones that can't fail still fit. |
| `!R(A)` is a function that returns `!R`. | The type reads as it is declared: `!void handle(Request *req)`. |
| Router middleware is a function that gives false to stop the request. | No closures needed, and the order is the order of `use`. |
| A server closes idle connections with one sweeper task per thread, which shuts down connections that did nothing for the timeout. | A timer for each read and write cost 15% of the throughput. The sweeper costs under 2%. |
| `Conn.setTimeout` gives each read and write its own deadline. | For clients, where one call waiting too long is what matters. |
| A generic struct's method is checked and compiled for a set of types when it is first used. | Methods like `contains` can need `==` without ruling out types that don't have it, and unused methods cost nothing. |
| `chan[T](0)` is a channel with no buffer, where a send waits for a receiver. | A hand-over that both tasks wait for, with no new syntax. |
| A WebSocket handler gets the connection and keeps it until it returns. | A chat or a feed is one function with a loop, and the router owns nothing after it. |
| `http.Client` sends a request again, on a new connection, when a kept connection turns out to be closed before any reply. | Servers close idle connections whenever they like. The caller shouldn't see it. |
| A CSV `Record` owns copies of its fields. | A quoted field with `""` in it has to be copied anyway, and records can outlive the text. |
| Command-line flags come from a `@flags` struct, as settings come from an `@env` struct. | The struct is the documentation: names, types, defaults and the help are in one place, and `--help` is made from it. |
| `flags.parse` ends the program on `--help` and on a wrong flag; `flags.parseArgs` gives errors instead. | A tool's `main` needs no code for either, and subcommands and tests can still decide. |
| Inside a module, a bare `error`, `format` or `print` is always the builtin, even when the module has a function of that name. | `log.error` and `term.error` keep their names, and the module's own code can still make errors. |
| A catch block keeps its own copy of the error's message. | Code in the block, like logging, can make and handle errors without changing `e.message`. |
| `term` writes colors as markup in the text: `[green]ok[/]`. | Colored output reads like the text it prints, and the same text works without colors. |
| Names are looked up by a resolver written in Iris, over the scheduler's UDP and TCP. The system's resolver is only used for names it can't find. | A lookup no longer holds up a whole thread, and names only the system knows (mDNS, a VPN's own servers) still work. |
| `lookup` asks for IPv4 and IPv6 at once and gives the IPv4 addresses first. | Half the time of asking in turn, and `dial` doesn't wait on IPv6 where there is none. |
| An address keeps an IPv4 address as an IPv6 one, `::ffff:a.b.c.d`. | One type and one size for both, as the system's own calls do. |
| HTTP/2 serves the same `Router` and handlers as HTTP/1.1, each request in a task of its own. | A program gets HTTP/2 without changes, and a slow handler holds up no other request. |
| Each HTTP/2 connection has one writer task, which alone writes to it. The reader and the requests hand it their frames. | No locks around the connection, flow control in one place, and a TLS connection is only used from its own thread. |
| The HPACK encoder adds nothing to the other side's table. | It needs no state and can't get out of step. The static table and literals keep responses small enough. |
| A `Client` uses HTTP/2 for `https` when the server offers it. | One connection carries all the requests to a host at once. |
| OpenSSL's calls hold SIGPIPE back on Linux. | A client that leaves at the wrong moment can't end the server. |
| Regular expressions run on a Pike VM, without backtracking. They use RE2 syntax, leftmost-first matches and RE2's rules for empty matches. | No pattern can take exponential time, and patterns written for RE2 work unchanged. |
| `regex` leaves out backreferences and lookaround. | They can't be matched in linear time. |
| Compression uses the system's zlib, loaded when first needed, as TLS uses OpenSSL. | zlib is on every system Iris runs on, and is fast and well tested. |
| Decompressing stops at a limit, 1 GiB unless another is given. | A few kilobytes of gzip can't fill the memory. |
| HTTP clients ask for gzip and decompress it themselves. Servers compress only when the Router asks. | Callers never see compressed bodies, and a server's output doesn't change without its author saying so. |
| The PostgreSQL driver speaks the protocol in Iris, without libpq. | A query waits like any other network call, so other tasks run meanwhile, and there is no C library to install. |
| `pg` sends values apart from the SQL and reads every result as text. | Values can't change what the SQL means, and text is the same for every type and every server version. |
| A query's rows are all read before `query` returns. | The connection goes back to the pool at once, and `Rows` can't hold one up. |
| `redis.Client` is a pool from the start. | Most programs share one client between tasks. One connection per task is never what they want. |
| A struct may hold a `List` of itself. | The list keeps its items behind a pointer, so trees and nested replies need no pointer games. |
| Text with values can be passed straight to a `str` parameter. It lives in a temporary `String` until the statement ends. | Calls like `res.text("hi {name}")` need no `format` and no `free`. |
| A task can't return an error. | Nothing waits for a task's result, so the error would be lost. |
| A panic exits with status 2. | Tells a crash apart from a program that returns 1. |
| A struct's field defaults apply wherever a value of it is made without one: variables, globals, arrays and literals. | A default that holds in some places and not others would be a trap. |
| Returning a pointer or slice into a function's own variables is an error. | The memory is gone once the function returns. |
| A global can't be set from another global. | Globals are set before `main` in no fixed order across modules. |
| A library page shows the module's comment, then its intro page with examples. | The comment says what the module is. The intro shows it in use. |
| Memory comes from the runtime's allocator (`iris_malloc`), or from an `Arena` that frees many values at once. An arena is not passed to `String`, `List` or the library. They keep using the heap. | One way to free each kind of value keeps ownership easy to follow. |

## Not in Iris

- Exceptions
- A preprocessor and macros
- Inheritance (structs can embed other structs instead)
- Function overloading
- `goto`
- `while` and `do`
- Untyped variable arguments, except when calling C
- A garbage collector
