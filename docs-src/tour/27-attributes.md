# Attributes

An attribute starts with `@` and goes on the line before a declaration. Some make the compiler write code for a struct: reading it from environment variables, from command-line flags, or from JSON. Others change how a function is compiled. This chapter covers each one except `@json`, which has the next chapter.

| Attribute | On | What it does |
|---|---|---|
| `@json` | a struct | turns it into JSON and back, with the `json` module |
| `@env` | a struct | fills it from environment variables, with the `env` module |
| `@flags` | a struct | fills it from the program's arguments, with the `flags` module |
| `@inline` | a function | always puts its code where it is called |
| `@unchecked` | a function | leaves out the checks of debug builds in its code |
| `@export` | a function | makes it callable from C by its name |

## @env

`@env` lets `env.load` fill a struct from environment variables. Each field reads the variable named after it in capitals, with `_` between its words: `dbUrl` reads `DB_URL`. A prefix on the struct goes in front of every name:

```iris run
import env

@env("SHOP")
struct Settings {
    u16 port = 8080         // SHOP_PORT
    str db = "shop.db"      // SHOP_DB
    Duration timeout = 30s  // SHOP_TIMEOUT
    bool debug              // SHOP_DEBUG
}

!void main() {
    scoped s := try env.load(Settings)
    print("{s.port} {s.db} {s.timeout} {s.debug}")
}
```

```output
8080 shop.db 30s false
```

Run with `SHOP_PORT=9000 SHOP_DEBUG=yes`, the same program prints `9000 shop.db 30s true`. A variable that isn't set leaves the field's default.

`@env("NAME")` on a field gives the variable's whole name, without the prefix, and `@env(required)` makes a missing variable an error; both can go together:

```iris
@env
struct Secrets {
    @env("DATABASE_URL", required) String db
    @env(required) str apiKey // API_KEY
}
```

Fields can be numbers, `bool` (`true`, `false`, `1`, `0`, `yes`, `no`, `on`, `off`), `str`, `String`, plain enums by their value's name, and `Duration` (`30s`, `1500ms`, `1m30s`). The errors are `env.EnvError.missing` and `invalid`, with the variable's name in the message:

```output
error: EnvError.missing: DATABASE_URL is needed but isn't set
```

An `@env` struct gets a `free` method, which frees its `String`s, so `scoped` works on it.

## @flags

`@flags` lets `flags.parse` fill a struct from the program's arguments. Each field is a flag named after it, with `-` between its words: `dryRun` is `--dry-run`. `@flag` on a field gives a letter for the short form, the help text, and `required`, in any order; a `str[]` field with `@flag(args)` gets the arguments that aren't flags:

```iris run
import flags

enum Format { text, json }

@flags("copies files into a folder")
struct Options {
    @flag('f', "overwrite files that are there") bool force
    @flag('j', "how many copies to run at once") i64 jobs = 4
    Format format
    @flag(required, "where the files go") str to
    @flag(args) str[] files
}

!void main() {
    scoped o := try flags.parseArgs(Options, ["-f", "--jobs=8", "--to", "/backup", "a.txt", "b.txt"], "copy")
    print("{o.force} {o.jobs} {o.format} {o.to} {o.files.len} files, the last {o.files[1]}")
}
```

```output
true 8 text /backup 2 files, the last b.txt
```

A program reads its own arguments with `flags.parse(Options)`, which ends the program when something is wrong, after saying what. `flags.parseArgs` takes the arguments to read and gives errors instead, for subcommands and tests.

Values can be written `--jobs 8`, `--jobs=8` or `-j 8`. A `bool` flag needs no value, and `--force=false` turns it off; one-letter `bool` flags can be joined, `-fv`. After `--`, every argument is taken as it is. Fields can have the same types as `@env` fields.

`-h` and `--help` print the help, made from the struct, and end the program:

```output
copies files into a folder

usage: copy [flags] [files...]

flags:
  -f, --force               overwrite files that are there
  -j, --jobs <n>            how many copies to run at once (default 4)
      --format <text|json>  (default text)
      --to <text>           where the files go (required)
  -h, --help                show this help
```

A wrong flag prints what is wrong and how to get the help, and the program ends with status 2. The [command-line guide](../guides/02-cli.html) builds a whole tool with `@flags`.

## @inline

`@inline` puts the function's code at every place it is called, instead of a call. It suits a tiny function on a path that runs very often:

```iris run
@inline
i64 square(i64 x) => x * x

void main() {
    print("{square(12)}")
}
```

```output
144
```

The optimiser already inlines small functions on its own in release builds; `@inline` makes sure of it.

## @unchecked

`@unchecked` leaves out the checks that debug builds add (indexes, overflow, `null`) from one function's code, in every build. It is for code that has been measured, whose indexes are known to be in range:

```iris run
@unchecked
u32 checksum(u8[] data) {
    u32 sum = 0
    for b in data {
        sum = sum * 31 + u32(b)
    }
    return sum
}

void main() {
    print("{checksum("hello")}")
}
```

```output
99162322
```

A mistake in such a function isn't caught: it reads or writes the wrong memory.

## @export

`@export` makes a function callable from C by its name, for a library that calls back into Iris code. `@export("name")` gives it another C name. An exported function takes and gives numbers and pointers, never `!T`, since C has no way to take an error (see [Calling C](29-calling-c.html)):

```iris
@export
i32 onRow(u8 *ctx, i32 n, u8 **values, u8 **names) {
    return 0
}
```
