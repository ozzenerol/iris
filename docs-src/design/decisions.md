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
