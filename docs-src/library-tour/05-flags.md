# Command-line flags

A struct marked `@flags` describes the flags a program takes, and the `flags` module fills it from the program's arguments. The compiler writes the table of flags for each struct, and the help is made from it.

## A first program

Each field is a flag named after it: `port` is `--port`. A field with a value, like `port = 8080`, keeps it when the flag isn't given. `flags.parse(Options)` reads the program's own arguments:

```iris
import flags

@flags("serves the files in a folder")
struct Options {
    u16 port = 8080
    str root = "."
    bool verbose
}

void main() {
    scoped o := flags.parse(Options)
    print("serving {o.root} on port {o.port}, verbose {o.verbose}")
}
```

Built as `serve`, the program prints:

```output
$ ./serve
serving . on port 8080, verbose false
$ ./serve --port 9000 --verbose --root /srv/www
serving /srv/www on port 9000, verbose true
```

`flags.parse` gives the struct itself. When something is wrong, it says what and ends the program, so `main` never sees a half-read struct.

## Reading given arguments

`flags.parseArgs(T, args, name)` reads the arguments given instead of the program's own, and gives a `!T` instead of ending the program. `name` is the program's name in the help and the messages; `""` uses the program's real name. The examples in this chapter use it, so they always run with the same arguments:

```iris run
import flags

@flags
struct Options {
    u16 port = 8080
    str root = "."
    bool verbose
}

!void main() {
    scoped o := try flags.parseArgs(Options, ["--port", "9000", "--verbose"], "serve")
    print("{o.root} {o.port} {o.verbose}")
}
```

```output
. 9000 true
```

It suits tests, and programs with subcommands (see below). A `str` field points into the arguments given, so they must live as long as the struct.

## Names and short forms

A field named with more than one word gets `-` between the words: `dryRun` is `--dry-run`. `@flag` on a field gives a letter for the short form and the help text, in any order:

```iris run
import flags

@flags("copies files into a folder")
struct Options {
    @flag('n', "show what would be copied") bool dryRun
    @flag('j', "how many copies to run at once") i64 jobs = 4
    @flag('o') str out = "backup"
}

!void main() {
    scoped a := try flags.parseArgs(Options, ["--dry-run", "--jobs", "8", "--out=/tmp/b"], "copy")
    print("{a.dryRun} {a.jobs} {a.out}")
    scoped b := try flags.parseArgs(Options, ["-n", "-j", "2", "-o", "x"], "copy")
    print("{b.dryRun} {b.jobs} {b.out}")
}
```

```output
true 8 /tmp/b
true 2 x
```

A value can be written `--jobs 8`, `--jobs=8` or `-j 8`.

## Bool flags

A `bool` flag needs no value: `--verbose` turns it on. `--verbose=false` turns it off, which is how a flag whose default is `true` is turned off. One-letter `bool` flags can be joined, `-vq` is `-v -q`:

```iris run
import flags

@flags
struct Options {
    @flag('v') bool verbose
    @flag('q') bool quiet
    bool color = true
}

!void main() {
    scoped o := try flags.parseArgs(Options, ["-vq", "--color=false"], "tool")
    print("{o.verbose} {o.quiet} {o.color}")
}
```

```output
true true false
```

A letter that needs a value can't be joined with others: `-vj 8` is an error.

## Arguments that aren't flags

A `str[]` field with `@flag(args)` gets the arguments that aren't flags, in order, wherever they are among the flags. After `--`, every argument is taken as it is, even one that starts with `-`:

```iris run
import flags

@flags
struct Options {
    @flag('f') bool force
    @flag(args) str[] files
}

!void main() {
    scoped o := try flags.parseArgs(Options, ["a.txt", "-f", "b.txt", "--", "-c.txt"], "rm")
    print("{o.force}, {o.files.len} files:")
    for f in o.files {
        print("  {f}")
    }
}
```

```output
true, 3 files:
  a.txt
  b.txt
  -c.txt
```

The slice belongs to the struct, so `scoped` frees it. Without an `@flag(args)` field, an argument that isn't a flag is an error.

## Required flags

`@flag(required)` makes a flag that must be given. On the `@flag(args)` field it means at least one argument is needed. `required` goes with the letter and the help in any order:

```iris run
import flags

@flags
struct Options {
    @flag('t', required, "where the files go") str to
    @flag(args, required) str[] files
}

void main() {
    _ = flags.parseArgs(Options, ["a.txt"], "copy") catch e {
        print(e.message)
        Options{}
    }
}
```

```output
--to is needed
usage: copy [flags] files...
run copy --help to see the flags

```

## Types

A field can be any integer or float type, `bool`, `str`, `String`, a plain enum, or `Duration`. An enum flag takes the name of one of its values, and a `Duration` flag takes `30s`, `1500ms` or `1m30s`:

```iris run
import flags

enum Format { text, json, csv }

@flags
struct Options {
    Format format
    Duration timeout = 30s
    f64 ratio = 0.5
    u8 level
    String label
}

!void main() {
    scoped o := try flags.parseArgs(Options, ["--format", "csv", "--timeout=1m30s", "--ratio", "0.25", "--level", "3", "--label", "night"], "report")
    print("{o.format} {o.timeout} {o.ratio} {o.level} {o.label}")
}
```

```output
csv 1m30s 0.25 3 night
```

A `String` field holds its own copy of the text, which the struct's `free` method frees. That method is what `scoped` calls.

## Help

`-h` and `--help` show the help, made from the struct: the text given to `@flags`, a usage line, and one line per flag with its short form, the kind of value it takes, its help and its default. `flags.parse` prints it and ends the program with status 0. `flags.parseArgs` fails with `flags.FlagError.help`, and the help is the message. Since every `@flags` struct has them, `'h'` can't be another flag's letter, and no field can be named `help`:

```iris run
import flags

enum Format { text, json }

@flags("copies files into a folder")
struct Options {
    @flag('f', "overwrite files that are there") bool force
    @flag('j', "how many copies to run at once") i64 jobs = 4
    Format format
    Duration timeout = 30s
    @flag(required, "where the files go") str to
    @flag(args) str[] files
}

void main() {
    _ = flags.parseArgs(Options, ["--help"], "copy") catch e {
        if e.code == flags.FlagError.help {
            print(e.message)
        }
        Options{}
    }
}
```

```output
copies files into a folder

usage: copy [flags] [files...]

flags:
  -f, --force               overwrite files that are there
  -j, --jobs <n>            how many copies to run at once (default 4)
      --format <text|json>  (default text)
      --timeout <duration>  (default 30s)
      --to <text>           where the files go (required)
  -h, --help                show this help

```

## Wrong flags

A flag that isn't there, a missing value, or a value of the wrong kind is an error. `flags.parseArgs` fails with `flags.FlagError.invalid`; the message says what is wrong, then gives the usage line and how to get the help:

```iris run
import flags

@flags
struct Options {
    @flag('j') i64 jobs = 4
    bool force
}

void check(str[] args) {
    _ = flags.parseArgs(Options, args, "copy") catch e {
        print("{e.code}: {e.message}")
        Options{}
    }
}

void main() {
    check(["--jobz", "2"])
    check(["--jobs", "many"])
    check(["-j"])
}
```

```output
FlagError.invalid: --jobz isn't a flag
usage: copy [flags]
run copy --help to see the flags

FlagError.invalid: --jobs: 'many' isn't a whole number
usage: copy [flags]
run copy --help to see the flags

FlagError.invalid: -j needs a value
usage: copy [flags]
run copy --help to see the flags

```

`flags.parse` prints the same message after `error: ` on standard error, and the program ends with status 2, the usual status for a wrong command line:

```output
$ ./copy --jobz 2
error: --jobz isn't a flag
usage: copy [flags]
run copy --help to see the flags
$ echo $?
2
```

## Subcommands

A program with subcommands, like `net ping` and `net scan`, has one `@flags` struct for each. The first argument picks the subcommand, and `flags.parseArgs` reads the rest with that subcommand's struct:

```iris run
import flags

@flags("sends echo requests to hosts")
struct PingOptions {
    @flag('c', "how many to send to each") i64 count = 4
    @flag(args, required) str[] hosts
}

@flags("finds the open ports of a host")
struct ScanOptions {
    @flag('t', "how long to wait for each port") Duration timeout = 1s
    @flag(args, required) str[] hosts
}

!void run(str[] args) {
    if args.len == 0 {
        print("usage: net ping|scan ...")
        return
    }
    rest := args[1:]
    match args[0] {
        "ping" => {
            scoped o := try flags.parseArgs(PingOptions, rest, "net ping")
            print("ping: {o.hosts.len} hosts, {o.count} each")
        }
        "scan" => {
            scoped o := try flags.parseArgs(ScanOptions, rest, "net scan")
            print("scan: {o.hosts[0]}, {o.timeout} a port")
        }
        _ => print("{args[0]} isn't a command")
    }
}

!void main() {
    try run(["ping", "-c", "2", "10.0.0.1", "10.0.0.2"])
    try run(["scan", "-t", "200ms", "10.0.0.5"])
}
```

```output
ping: 2 hosts, 2 each
scan: 10.0.0.5, 200ms a port
```

A real program builds `args` from its own arguments, with `os.argc()` and `os.arg(i)`.

## The raw arguments

The `os` module gives the arguments as they are, without flags: `os.argc()` is how many there are, and `os.arg(i)` gives one, `os.arg(0)` being the program's own name:

```iris
import os

void main() {
    for i in 0..os.argc() {
        print("argument {i}: {os.arg(i)}")
    }
}
```
