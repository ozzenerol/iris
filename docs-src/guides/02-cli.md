# Build a command-line tool

This guide builds `sizes` from an empty folder: a tool that shows what takes the space in a folder. It reads flags, lines its output up in a table with colors, shows a progress bar, and can ask for folders one after another with an editable prompt. Each step ends with a program that runs, and the commands to try it.

## Before you start

You need Iris: `iris version` prints its version.

Make a folder for the tool and go into it:

```shell
mkdir sizes
cd sizes
```

Everything in this guide happens in this folder. The program is one file, `main.iris`, which each step changes.

The tool needs something to measure. These commands make a folder called `demo` with files of known sizes: `head -c 4404019 /dev/zero` writes that many zero bytes. One name has brackets in it, which matters in the step about colors.

```shell try
mkdir -p demo/photos demo/code
head -c 4404019 /dev/zero > demo/video.mp4
head -c 1258291 /dev/zero > demo/photos/beach.jpg
head -c 921600 /dev/zero > demo/photos/hills.jpg
head -c 40960 /dev/zero > demo/code/main.c
head -c 12288 /dev/zero > demo/notes.txt
head -c 3072 /dev/zero > "demo/[draft].md"
head -c 2048 /dev/zero > demo/.cache
```

## Flags

Create `main.iris`. A struct with `@flags` describes the flags, and `flags.parse` fills it from the program's arguments. Each field is a flag named after it: `top` is `--top`. `@flag` gives a letter for the short form and the help text. The `str[]` field with `@flag(args)` gets the arguments that aren't flags, here the folders to measure. A field with a value, like `top = 10`, keeps it when the flag isn't given. The [Command-line flags](../tour/30-flags.html) chapter has the rest.

```iris
// sizes: what takes the space in a folder.
```

```iris
import flags
```

```iris
@flags("shows what takes the space in a folder")
struct Options {
    @flag('n', "how many entries to show") i64 top = 10
    @flag('a', "count names that start with a dot") bool all
    @flag(args) str[] folders
}
```

For now `main` only prints what it read:

```iris
void main() {
    opts := flags.parse(Options)
    print("top {opts.top}, all {opts.all}, {opts.folders.len} folders")
}
```

```iris step
// sizes: what takes the space in a folder.

import flags

@flags("shows what takes the space in a folder")
struct Options {
    @flag('n', "how many entries to show") i64 top = 10
    @flag('a', "count names that start with a dot") bool all
    @flag(args) str[] folders
}

void main() {
    opts := flags.parse(Options)
    print("top {opts.top}, all {opts.all}, {opts.folders.len} folders")
}
```

`iris build` makes a program called `sizes` in the folder; `./sizes` runs it. Try it with good flags and a wrong one:

```shell try
iris build main.iris -o sizes
./sizes -n 3 demo
./sizes --top x
```

```output
top 3, all false, 1 folders
error: --top: 'x' isn't a whole number
usage: sizes [flags] [folders...]
run sizes --help to see the flags
```

A wrong flag ends the program with status 2. `--help` prints the help, made from the struct:

```shell try
./sizes --help
```

```output
shows what takes the space in a folder

usage: sizes [flags] [folders...]

flags:
  -n, --top <n>   how many entries to show (default 10)
  -a, --all       count names that start with a dot
  -h, --help      show this help
```

## Adding up the sizes

The tool now measures each entry of a folder and prints the biggest first. The import line gets `fs` for files and folders, `path` to join names, and `str`:

```iris
import flags, fs, path, str
```

`Item` is one entry: its name, its size, and whether it is a folder. `Item.free` frees the name. It is `pub` because `List.freeEach`, in the standard library, calls it.

```iris
struct Item {
    String name
    i64 size
    bool isDir
}

pub void Item.free() {
    self.name.free()
}
```

The options move to a global, so every function can read them:

```iris
Options opts
```

`sizeOf` gives the bytes in a file, or in a folder and everything in it, by calling itself for each entry. A file or folder it can't read counts as 0.

```iris
// The bytes in p: a file, or a folder and everything in it.
i64 sizeOf(str p) {
    info := fs.stat(p) catch e {
        return 0
    }
    if !info.isDir {
        return info.size
    }
    scoped fs.Dir dir = fs.readDir(p) catch e {
        return 0
    }
    i64 total = 0
    for e in dir.entries {
        scoped String inside = path.join(p, e.name)
        total += sizeOf(inside)
    }
    return total
}
```

`human` writes a size as people read it, with one decimal:

```iris
// 1536 is "1.5 KiB".
String human(i64 n) {
    if n < 1024 {
        return format("{n} B")
    }
    units := ["KiB", "MiB", "GiB", "TiB"]
    i64 scaled = n * 10 / 1024
    i64 u = 0
    for scaled >= 10240 && u < 3 {
        scaled /= 1024
        u += 1
    }
    return format("{scaled / 10}.{scaled % 10} {units[u]}")
}
```

`show` reads the folder, adds up each entry, sorts them with the biggest first, and prints the first `opts.top`. Names that start with a dot are skipped unless `--all` is given. `bigger` is the order for `sort`.

```iris
bool bigger(Item a, Item b) => a.size > b.size
```

```iris
!void show(str folder) {
    scoped fs.Dir dir = try fs.readDir(folder)
    List[Item] items
    defer items.freeEach()
    i64 total = 0
    for e in dir.entries {
        if !opts.all && str.startsWith(e.name, ".") {
            continue
        }
        scoped String full = path.join(folder, e.name)
        size := sizeOf(full)
        total += size
        items.push(Item{name: e.name.copy(), size: size, isDir: e.isDir})
    }
    items.sort(bigger)
    for i in 0..items.len {
        if i == opts.top {
            break
        }
        it := items.at(i)
        scoped String size = human(it.size)
        print("{size}  {it.name}")
    }
    scoped String sum = human(total)
    print("{folder}: {sum} in {items.len} entries")
}
```

`main` fills the global and shows each folder, or the current one when none is given:

```iris
!void main() {
    opts = flags.parse(Options)
    if opts.folders.len == 0 {
        try show(".")
        return
    }
    for f in opts.folders {
        try show(f)
    }
}
```

```iris step
// sizes: what takes the space in a folder.

import flags, fs, path, str

@flags("shows what takes the space in a folder")
struct Options {
    @flag('n', "how many entries to show") i64 top = 10
    @flag('a', "count names that start with a dot") bool all
    @flag(args) str[] folders
}

struct Item {
    String name
    i64 size
    bool isDir
}

pub void Item.free() {
    self.name.free()
}

Options opts

// The bytes in p: a file, or a folder and everything in it.
i64 sizeOf(str p) {
    info := fs.stat(p) catch e {
        return 0
    }
    if !info.isDir {
        return info.size
    }
    scoped fs.Dir dir = fs.readDir(p) catch e {
        return 0
    }
    i64 total = 0
    for e in dir.entries {
        scoped String inside = path.join(p, e.name)
        total += sizeOf(inside)
    }
    return total
}

// 1536 is "1.5 KiB".
String human(i64 n) {
    if n < 1024 {
        return format("{n} B")
    }
    units := ["KiB", "MiB", "GiB", "TiB"]
    i64 scaled = n * 10 / 1024
    i64 u = 0
    for scaled >= 10240 && u < 3 {
        scaled /= 1024
        u += 1
    }
    return format("{scaled / 10}.{scaled % 10} {units[u]}")
}

bool bigger(Item a, Item b) => a.size > b.size

!void show(str folder) {
    scoped fs.Dir dir = try fs.readDir(folder)
    List[Item] items
    defer items.freeEach()
    i64 total = 0
    for e in dir.entries {
        if !opts.all && str.startsWith(e.name, ".") {
            continue
        }
        scoped String full = path.join(folder, e.name)
        size := sizeOf(full)
        total += size
        items.push(Item{name: e.name.copy(), size: size, isDir: e.isDir})
    }
    items.sort(bigger)
    for i in 0..items.len {
        if i == opts.top {
            break
        }
        it := items.at(i)
        scoped String size = human(it.size)
        print("{size}  {it.name}")
    }
    scoped String sum = human(total)
    print("{folder}: {sum} in {items.len} entries")
}

!void main() {
    opts = flags.parse(Options)
    if opts.folders.len == 0 {
        try show(".")
        return
    }
    for f in opts.folders {
        try show(f)
    }
}
```

```shell try
iris build main.iris -o sizes
./sizes demo
./sizes -n 2 --all demo
```

```output
4.1 MiB  video.mp4
2.0 MiB  photos
40.0 KiB  code
12.0 KiB  notes.txt
3.0 KiB  [draft].md
demo: 6.3 MiB in 5 entries
4.1 MiB  video.mp4
2.0 MiB  photos
demo: 6.3 MiB in 6 entries
```

## A table and colors

`term.table` lines the columns up. A column of numbers lines up on the right; sizes like `4.1 MiB` aren't numbers, so `t.align` puts that column on the right. Text for `term` can hold markup: `[blue]...[/]` colors the folders. A name from the disk could hold a `[`, like `[draft].md`, so `term.escape` writes it as `[[` first, and it shows as it is.

`term.success` and `term.error` print a line after a green ✓ or a red ✗. Colors are used only on a terminal: in a pipe or a file, or when `NO_COLOR` is set, the markup is taken out.

The import line gets `os` and `term`:

```iris
import flags, fs, os, path, str, term
```

`show` prints a table instead of lines:

```iris
!void show(str folder) {
    scoped fs.Dir dir = try fs.readDir(folder)
    List[Item] items
    defer items.freeEach()
    i64 total = 0
    for e in dir.entries {
        if !opts.all && str.startsWith(e.name, ".") {
            continue
        }
        scoped String full = path.join(folder, e.name)
        size := sizeOf(full)
        total += size
        items.push(Item{name: e.name.copy(), size: size, isDir: e.isDir})
    }
    items.sort(bigger)

    scoped t := term.table(["SIZE", "NAME"])
    t.align(0, .right)
    for i in 0..items.len {
        if i == opts.top {
            break
        }
        it := items.at(i)
        scoped String size = human(it.size)
        scoped String name = term.escape(it.name)
        scoped String shown = if it.isDir { format("[blue]{name}/[/]") } else { name.copy() }
        t.row([size, shown])
    }
    t.print()
    scoped String sum = human(total)
    term.success("{folder}: {sum} in {items.len} entries")
}
```

`main` shows a folder that can't be read as an error, and exits with status 1:

```iris
!void main() {
    opts = flags.parse(Options)
    if opts.folders.len == 0 {
        try show(".")
        return
    }
    for f in opts.folders {
        show(f) catch e {
            term.error(e.message)
            os.exit(1)
        }
    }
}
```

```iris step
// sizes: what takes the space in a folder.

import flags, fs, os, path, str, term

@flags("shows what takes the space in a folder")
struct Options {
    @flag('n', "how many entries to show") i64 top = 10
    @flag('a', "count names that start with a dot") bool all
    @flag(args) str[] folders
}

struct Item {
    String name
    i64 size
    bool isDir
}

pub void Item.free() {
    self.name.free()
}

Options opts

// The bytes in p: a file, or a folder and everything in it.
i64 sizeOf(str p) {
    info := fs.stat(p) catch e {
        return 0
    }
    if !info.isDir {
        return info.size
    }
    scoped fs.Dir dir = fs.readDir(p) catch e {
        return 0
    }
    i64 total = 0
    for e in dir.entries {
        scoped String inside = path.join(p, e.name)
        total += sizeOf(inside)
    }
    return total
}

// 1536 is "1.5 KiB".
String human(i64 n) {
    if n < 1024 {
        return format("{n} B")
    }
    units := ["KiB", "MiB", "GiB", "TiB"]
    i64 scaled = n * 10 / 1024
    i64 u = 0
    for scaled >= 10240 && u < 3 {
        scaled /= 1024
        u += 1
    }
    return format("{scaled / 10}.{scaled % 10} {units[u]}")
}

bool bigger(Item a, Item b) => a.size > b.size

!void show(str folder) {
    scoped fs.Dir dir = try fs.readDir(folder)
    List[Item] items
    defer items.freeEach()
    i64 total = 0
    for e in dir.entries {
        if !opts.all && str.startsWith(e.name, ".") {
            continue
        }
        scoped String full = path.join(folder, e.name)
        size := sizeOf(full)
        total += size
        items.push(Item{name: e.name.copy(), size: size, isDir: e.isDir})
    }
    items.sort(bigger)

    scoped t := term.table(["SIZE", "NAME"])
    t.align(0, .right)
    for i in 0..items.len {
        if i == opts.top {
            break
        }
        it := items.at(i)
        scoped String size = human(it.size)
        scoped String name = term.escape(it.name)
        scoped String shown = if it.isDir { format("[blue]{name}/[/]") } else { name.copy() }
        t.row([size, shown])
    }
    t.print()
    scoped String sum = human(total)
    term.success("{folder}: {sum} in {items.len} entries")
}

!void main() {
    opts = flags.parse(Options)
    if opts.folders.len == 0 {
        try show(".")
        return
    }
    for f in opts.folders {
        show(f) catch e {
            term.error(e.message)
            os.exit(1)
        }
    }
}
```

```shell try
iris build main.iris -o sizes
./sizes demo
./sizes /nope
```

```output
    SIZE  NAME
 4.1 MiB  video.mp4
 2.0 MiB  photos/
40.0 KiB  code/
12.0 KiB  notes.txt
 3.0 KiB  [draft].md
✓ demo: 6.3 MiB in 5 entries
✗ /nope: no such file
```

## A progress bar

Adding up a big folder takes a while. `term.progress` draws a bar on stderr, drawn again in place as `p.add()` is called. Lines printed with `term` while it runs go above it. `p.done()` writes the last line, with the time it took. When stderr isn't a terminal, only that last line is written.

```iris
!void show(str folder) {
    scoped fs.Dir dir = try fs.readDir(folder)
    List[Item] items
    defer items.freeEach()
    p := term.progress(folder, dir.entries.len)
    i64 total = 0
    for e in dir.entries {
        p.add()
        if !opts.all && str.startsWith(e.name, ".") {
            continue
        }
        scoped String full = path.join(folder, e.name)
        size := sizeOf(full)
        total += size
        items.push(Item{name: e.name.copy(), size: size, isDir: e.isDir})
    }
    p.done()
    items.sort(bigger)

    scoped t := term.table(["SIZE", "NAME"])
    t.align(0, .right)
    for i in 0..items.len {
        if i == opts.top {
            break
        }
        it := items.at(i)
        scoped String size = human(it.size)
        scoped String name = term.escape(it.name)
        scoped String shown = if it.isDir { format("[blue]{name}/[/]") } else { name.copy() }
        t.row([size, shown])
    }
    t.print()
    scoped String sum = human(total)
    term.success("{folder}: {sum} in {items.len} entries")
}
```

```iris step
// sizes: what takes the space in a folder.

import flags, fs, os, path, str, term

@flags("shows what takes the space in a folder")
struct Options {
    @flag('n', "how many entries to show") i64 top = 10
    @flag('a', "count names that start with a dot") bool all
    @flag(args) str[] folders
}

struct Item {
    String name
    i64 size
    bool isDir
}

pub void Item.free() {
    self.name.free()
}

Options opts

// The bytes in p: a file, or a folder and everything in it.
i64 sizeOf(str p) {
    info := fs.stat(p) catch e {
        return 0
    }
    if !info.isDir {
        return info.size
    }
    scoped fs.Dir dir = fs.readDir(p) catch e {
        return 0
    }
    i64 total = 0
    for e in dir.entries {
        scoped String inside = path.join(p, e.name)
        total += sizeOf(inside)
    }
    return total
}

// 1536 is "1.5 KiB".
String human(i64 n) {
    if n < 1024 {
        return format("{n} B")
    }
    units := ["KiB", "MiB", "GiB", "TiB"]
    i64 scaled = n * 10 / 1024
    i64 u = 0
    for scaled >= 10240 && u < 3 {
        scaled /= 1024
        u += 1
    }
    return format("{scaled / 10}.{scaled % 10} {units[u]}")
}

bool bigger(Item a, Item b) => a.size > b.size

!void show(str folder) {
    scoped fs.Dir dir = try fs.readDir(folder)
    List[Item] items
    defer items.freeEach()
    p := term.progress(folder, dir.entries.len)
    i64 total = 0
    for e in dir.entries {
        p.add()
        if !opts.all && str.startsWith(e.name, ".") {
            continue
        }
        scoped String full = path.join(folder, e.name)
        size := sizeOf(full)
        total += size
        items.push(Item{name: e.name.copy(), size: size, isDir: e.isDir})
    }
    p.done()
    items.sort(bigger)

    scoped t := term.table(["SIZE", "NAME"])
    t.align(0, .right)
    for i in 0..items.len {
        if i == opts.top {
            break
        }
        it := items.at(i)
        scoped String size = human(it.size)
        scoped String name = term.escape(it.name)
        scoped String shown = if it.isDir { format("[blue]{name}/[/]") } else { name.copy() }
        t.row([size, shown])
    }
    t.print()
    scoped String sum = human(total)
    term.success("{folder}: {sum} in {items.len} entries")
}

!void main() {
    opts = flags.parse(Options)
    if opts.folders.len == 0 {
        try show(".")
        return
    }
    for f in opts.folders {
        show(f) catch e {
            term.error(e.message)
            os.exit(1)
        }
    }
}
```

```shell try
iris build main.iris -o sizes
./sizes demo
```

```output
demo  ██████████████████████████████  100%  6/6  …
    SIZE  NAME
 4.1 MiB  video.mp4
 2.0 MiB  photos/
40.0 KiB  code/
12.0 KiB  notes.txt
 3.0 KiB  [draft].md
✓ demo: 6.3 MiB in 5 entries
```

## Asking for folders

With `-i`, `sizes` asks for one folder after another. The new flag goes in `Options`:

```iris
@flags("shows what takes the space in a folder")
struct Options {
    @flag('n', "how many entries to show") i64 top = 10
    @flag('a', "count names that start with a dot") bool all
    @flag('i', "ask for folders, one after another") bool interactive
    @flag(args) str[] folders
}
```

`term.readLine` shows a prompt and lets the line be edited with the usual keys: arrows, Home and End, Ctrl-W to cut a word, Up and Down for the lines typed before. `term.historyFile` keeps those lines in a file in your home folder between runs. At Ctrl-D, `readLine` fails, which ends the loop.

```iris
void shell() {
    home := os.env("HOME") catch e { "." }
    scoped String history = path.join(home, ".sizes_history")
    term.historyFile(history)
    term.info("type a folder to see what is in it; Ctrl-D ends")
    for {
        line := term.readLine("[bold]folder>[/] ") catch e {
            return
        }
        folder := str.trim(line)
        if folder.len > 0 {
            show(folder) catch e {
                term.error(e.message)
            }
        }
        line.free()
    }
}
```

`main` calls `shell` when `-i` is given:

```iris
!void main() {
    opts = flags.parse(Options)
    if opts.interactive {
        shell()
        return
    }
    if opts.folders.len == 0 {
        try show(".")
        return
    }
    for f in opts.folders {
        show(f) catch e {
            term.error(e.message)
            os.exit(1)
        }
    }
}
```

```iris step
// sizes: what takes the space in a folder.

import flags, fs, os, path, str, term

@flags("shows what takes the space in a folder")
struct Options {
    @flag('n', "how many entries to show") i64 top = 10
    @flag('a', "count names that start with a dot") bool all
    @flag('i', "ask for folders, one after another") bool interactive
    @flag(args) str[] folders
}

struct Item {
    String name
    i64 size
    bool isDir
}

pub void Item.free() {
    self.name.free()
}

Options opts

// The bytes in p: a file, or a folder and everything in it.
i64 sizeOf(str p) {
    info := fs.stat(p) catch e {
        return 0
    }
    if !info.isDir {
        return info.size
    }
    scoped fs.Dir dir = fs.readDir(p) catch e {
        return 0
    }
    i64 total = 0
    for e in dir.entries {
        scoped String inside = path.join(p, e.name)
        total += sizeOf(inside)
    }
    return total
}

// 1536 is "1.5 KiB".
String human(i64 n) {
    if n < 1024 {
        return format("{n} B")
    }
    units := ["KiB", "MiB", "GiB", "TiB"]
    i64 scaled = n * 10 / 1024
    i64 u = 0
    for scaled >= 10240 && u < 3 {
        scaled /= 1024
        u += 1
    }
    return format("{scaled / 10}.{scaled % 10} {units[u]}")
}

bool bigger(Item a, Item b) => a.size > b.size

!void show(str folder) {
    scoped fs.Dir dir = try fs.readDir(folder)
    List[Item] items
    defer items.freeEach()
    p := term.progress(folder, dir.entries.len)
    i64 total = 0
    for e in dir.entries {
        p.add()
        if !opts.all && str.startsWith(e.name, ".") {
            continue
        }
        scoped String full = path.join(folder, e.name)
        size := sizeOf(full)
        total += size
        items.push(Item{name: e.name.copy(), size: size, isDir: e.isDir})
    }
    p.done()
    items.sort(bigger)

    scoped t := term.table(["SIZE", "NAME"])
    t.align(0, .right)
    for i in 0..items.len {
        if i == opts.top {
            break
        }
        it := items.at(i)
        scoped String size = human(it.size)
        scoped String name = term.escape(it.name)
        scoped String shown = if it.isDir { format("[blue]{name}/[/]") } else { name.copy() }
        t.row([size, shown])
    }
    t.print()
    scoped String sum = human(total)
    term.success("{folder}: {sum} in {items.len} entries")
}

void shell() {
    home := os.env("HOME") catch e { "." }
    scoped String history = path.join(home, ".sizes_history")
    term.historyFile(history)
    term.info("type a folder to see what is in it; Ctrl-D ends")
    for {
        line := term.readLine("[bold]folder>[/] ") catch e {
            return
        }
        folder := str.trim(line)
        if folder.len > 0 {
            show(folder) catch e {
                term.error(e.message)
            }
        }
        line.free()
    }
}

!void main() {
    opts = flags.parse(Options)
    if opts.interactive {
        shell()
        return
    }
    if opts.folders.len == 0 {
        try show(".")
        return
    }
    for f in opts.folders {
        show(f) catch e {
            term.error(e.message)
            os.exit(1)
        }
    }
}
```

Build it and run it with `-i`, type `demo/photos`, then press Ctrl-D:

```shell
iris build main.iris -o sizes
./sizes -i
```

```output
• type a folder to see what is in it; Ctrl-D ends
folder> demo/photos
demo/photos  ██████████████████████████████  100%  2/2  0.0s
     SIZE  NAME
  1.1 MiB  beach.jpg
900.0 KiB  hills.jpg
✓ demo/photos: 2.0 MiB in 2 entries
folder>
```

When its input is a pipe, `readLine` reads plain lines and shows no prompt, so the same program also takes folders from another program:

```shell try
iris build main.iris -o sizes
printf 'demo/photos\ndemo/code\n' | ./sizes -i
```

```output
• type a folder to see what is in it; Ctrl-D ends
demo/photos  ██████████████████████████████  100%  2/2  …
     SIZE  NAME
  1.1 MiB  beach.jpg
900.0 KiB  hills.jpg
✓ demo/photos: 2.0 MiB in 2 entries
demo/code  ██████████████████████████████  100%  1/1  …
    SIZE  NAME
40.0 KiB  main.c
✓ demo/code: 40.0 KiB in 1 entries
```

## The whole program

```iris whole
// sizes: what takes the space in a folder.

import flags, fs, os, path, str, term

@flags("shows what takes the space in a folder")
struct Options {
    @flag('n', "how many entries to show") i64 top = 10
    @flag('a', "count names that start with a dot") bool all
    @flag('i', "ask for folders, one after another") bool interactive
    @flag(args) str[] folders
}

struct Item {
    String name
    i64 size
    bool isDir
}

pub void Item.free() {
    self.name.free()
}

Options opts

// The bytes in p: a file, or a folder and everything in it.
i64 sizeOf(str p) {
    info := fs.stat(p) catch e {
        return 0
    }
    if !info.isDir {
        return info.size
    }
    scoped fs.Dir dir = fs.readDir(p) catch e {
        return 0
    }
    i64 total = 0
    for e in dir.entries {
        scoped String inside = path.join(p, e.name)
        total += sizeOf(inside)
    }
    return total
}

// 1536 is "1.5 KiB".
String human(i64 n) {
    if n < 1024 {
        return format("{n} B")
    }
    units := ["KiB", "MiB", "GiB", "TiB"]
    i64 scaled = n * 10 / 1024
    i64 u = 0
    for scaled >= 10240 && u < 3 {
        scaled /= 1024
        u += 1
    }
    return format("{scaled / 10}.{scaled % 10} {units[u]}")
}

bool bigger(Item a, Item b) => a.size > b.size

!void show(str folder) {
    scoped fs.Dir dir = try fs.readDir(folder)
    List[Item] items
    defer items.freeEach()
    p := term.progress(folder, dir.entries.len)
    i64 total = 0
    for e in dir.entries {
        p.add()
        if !opts.all && str.startsWith(e.name, ".") {
            continue
        }
        scoped String full = path.join(folder, e.name)
        size := sizeOf(full)
        total += size
        items.push(Item{name: e.name.copy(), size: size, isDir: e.isDir})
    }
    p.done()
    items.sort(bigger)

    scoped t := term.table(["SIZE", "NAME"])
    t.align(0, .right)
    for i in 0..items.len {
        if i == opts.top {
            break
        }
        it := items.at(i)
        scoped String size = human(it.size)
        scoped String name = term.escape(it.name)
        scoped String shown = if it.isDir { format("[blue]{name}/[/]") } else { name.copy() }
        t.row([size, shown])
    }
    t.print()
    scoped String sum = human(total)
    term.success("{folder}: {sum} in {items.len} entries")
}

void shell() {
    home := os.env("HOME") catch e { "." }
    scoped String history = path.join(home, ".sizes_history")
    term.historyFile(history)
    term.info("type a folder to see what is in it; Ctrl-D ends")
    for {
        line := term.readLine("[bold]folder>[/] ") catch e {
            return
        }
        folder := str.trim(line)
        if folder.len > 0 {
            show(folder) catch e {
                term.error(e.message)
            }
        }
        line.free()
    }
}

!void main() {
    opts = flags.parse(Options)
    if opts.interactive {
        shell()
        return
    }
    if opts.folders.len == 0 {
        try show(".")
        return
    }
    for f in opts.folders {
        show(f) catch e {
            term.error(e.message)
            os.exit(1)
        }
    }
}
```

## Next

`iris build --mode=release-safe main.iris -o sizes` makes the program faster; copy `sizes` to a folder on your `PATH` to use it anywhere. The [term library page](../std/index.html#term) has the rest of the markup, tables and the line editor, and the [Attributes chapter](../tour/27-attributes.html) has every `@flags` option.
