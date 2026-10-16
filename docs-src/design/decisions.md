# Decisions

The choices made so far, the ones proposed and waiting for an answer, and the features Iris does not have.

## Settled

| Decision | Why |
|---|---|
| The compiler is written in C. | It builds fast and LLVM has a C API. C++ takes too long to compile. |
| Declarations put the type first: `i64 n = 5`. `n := 5` takes the type from the value. | Familiar from C. `:=` keeps short code short. |
| A function that can fail returns `!T`. Errors are handled with `try`, `catch` and `errdefer`. | Every place that can fail is visible, and errors cost nothing when nothing fails. |
| No optional types and no `?` syntax. | One more idea to learn for little gain. |
| No lambdas. Callbacks are named functions. | Same reason. Without a garbage collector, capturing variables also needs rules. |
| No semicolons. Blocks need braces. Conditions need no parentheses. | Less to type, no dangling `else`. |
| `for` is the only loop. | `while` and `do` add nothing `for` can't do. |
| `match` replaces `switch`. `if` and `match` give values. | No fall-through bugs, and no need for a separate `?:` operator. |
| No garbage collector. | Predictable memory use and latency. |

## Proposed

These are suggestions. They are used in the examples on these pages until they are settled.

| Proposal | Reason |
|---|---|
| No ownership checks for now. Memory is handled with `defer`, `scoped` and the debug-build checks. | A check that each value is freed once can be added later without breaking code. |
| Any pointer can be `null`. Debug builds check every use. | Same as C. Non-null pointers would need their own syntax. |
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
| Arithmetic happens in the operands' type, with no promotion to a wider type. | The type of every result is visible, and overflow checks catch what C would silently widen. |
| A struct field's default value must be known when compiling. | The default is copied into every literal that leaves the field out. |
| `str` and `u8[]` are one type. | Text is bytes; one type needs one set of rules. |
| A `String` can be used where a `str` is expected. | Printing, comparing and slicing an owned string need no conversion. |
| `e.code` can be compared with a value of any enum. | Errors from different parts of a program use different enums. |
| An error that leaves `main` prints its code, message and trace, and exits with status 1. | Status 2 stays for panics. |
| Plain enums come in 0.4, with errors; enums with data in 0.5. | Error codes are enum values. |
| Values of an enum with data can't be compared with `==`. | What equality means for the data is unclear; `match` says it exactly. |
| A function's several values must be unpacked into names. | No tuple variables and no `.0` syntax: one less thing to learn. |
| Only structs have methods, so only structs satisfy interfaces. | Integers stay plain values; a generic function over numbers needs no bound. |
| A generic struct's types are always written out: `List[str] names`. There are no static methods like `List[str].new()`. | One way to make a value: a declaration or a literal. |
| A generic function is checked for each set of types it is used with, not on its own. | Errors point at the generic code and at the use that caused them. |
| A folder with `iris.mod` is a project, and each of its folders is a module. A file outside a project is a program on its own. | Projects get modules; small programs and scripts stay one file. |
| A package's version is a tag in its repository, and `iris get` downloads it with git. | No package server to run. |
| Until `import c`, the standard library reaches C through `@extern("name")` on functions without a body, which only the standard library may use. | Keeps C out of user code until there is a proper way in. |
| `.ptr` gives a slice's pointer, and `p[0:n]` slices a pointer without checks. | Needed to hand memory to and from C. |
| `import c` turns `void *` and function pointers into `u8 *`, and `char` into `u8`. | Text and memory are bytes in Iris; callbacks come later. |
| A C name that is an Iris keyword or built-in name gets a `_`: `c.free_`. A struct named like a function becomes `c.struct_NAME`. | C keeps struct names apart from other names; Iris doesn't. |
| An integer constant can be used where a float is expected; an integer variable must be converted with `f64(...)`. | `x * 2` reads naturally; mixing variables stays explicit, as with integers. |
| Floats print in the fewest digits that read back the same, as plain decimals between 1e-7 and 1e21. | Printing is exact without being noisy. |
| `u8 *` converts to and from any pointer on its own. | It is what C's `void *` becomes, and C hands those around. |
| Function values are named functions only: no lambdas and no closures. | No hidden allocation for captured variables; a context pointer does the job, as in C. |
| Each thread has its own event loop and its tasks never move to another thread. `http.serve` runs one loop per core. | A task needs no locks for its own data, and a server still uses every core. |
| `thread f(args)` starts a thread and gives a `Thread` to join, written like `spawn`. | One way to start work, whether it is a task or a thread. |
| A thread ends when its function and the tasks it started have finished. | `join` waits for all the work the thread was given. |
| Channels work between threads. | They are the simplest way to hand work from one thread to another. |
| `Atomic[T]` is a type used only through its methods, for integers and `bool`. All operations are sequentially consistent. | A plain read or write of it can't happen by mistake, and there are no memory orders to learn. |
| `Mutex` has `lock`, `unlock` and `tryLock`, used with `defer`. A task waiting for it lets the other tasks of its thread run. | Nothing new to learn, and a waiting task doesn't stop its thread. |
| `threadlocal` is for global variables only. | A local variable belongs to one task already. |
| `@json` works from a table the compiler writes for each type; the runtime encodes, decodes and frees from it. | One small piece of C serves every type, and the program stays small. |
| A JSON key is the field's name as written. A missing field keeps its default unless it is `@json(required)`. Unknown keys are skipped. | Matches what most APIs send, with one attribute for the strict case. |
| A `@json` struct without a `free` method gets one. | Decoded values work with `scoped`. |
| A `String` from a call, passed straight to a `str` parameter, is freed when the statement ends. | `res.json(json.encode(u))` needs no variable and doesn't leak. Same rule as text with values. |
| `r"""..."""` is raw text over several lines. | JSON and other text with quotes and braces can be written as it is. |
| Text with values can be passed straight to a `str` parameter. It lives in a temporary `String` until the statement ends. | Calls like `res.text("hi {name}")` need no `format` and no `free`. |
| A channel has room for at least one value. | Sending into a full channel waits; there is no hand-over without room yet. |
| A task can't return an error. | Nothing waits for a task's result, so the error would be lost. |
| A panic exits with status 2. | Tells a crash apart from a program that returns 1. |
| Standard library functions that allocate take an allocator as their last parameter, defaulting to the general one. | Most calls stay short, and an arena can be passed when needed. |

## Not in Iris

- Exceptions
- A preprocessor and macros
- Inheritance (structs can embed other structs instead)
- Function overloading
- `goto`
- `while` and `do`
- Untyped variable arguments, except when calling C
- A garbage collector
