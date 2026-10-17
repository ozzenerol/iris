# Build a command-line tool

This guide builds `sizes`, a tool that shows what takes the space in a folder. It reads flags, lines its output up in a table with colors, shows a progress bar, and can ask for folders one after another with an editable prompt. Each step shows every function and declaration it adds or changes, in full. The last section has the whole program.

The guide's examples run it on a folder called `demo`.

## Flags

A struct with `@flags` describes the flags. `flags.parse` fills it from the program's arguments. Each field is a flag named after it. `@flag` gives a letter for the short form and the help, and the `str[]` field with `@flag(args)` gets the arguments that aren't flags.

```iris
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

```shell
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

A wrong flag ends the program with status 2. `--help` prints the help made from the struct:

```output
shows what takes the space in a folder

usage: sizes [flags] [folders...]

flags:
  -n, --top <n>   how many entries to show (default 10)
  -a, --all       count names that start with a dot
  -h, --help      show this help
```

## Adding up the sizes

`sizeOf` gives the bytes in a file, or in a folder and everything in it. A file or folder it can't read counts as 0.

```iris
import flags, fs, path, str
```

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

```iris
Options opts
```

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

`show` reads the folder, adds up each entry, sorts them with the biggest first and prints the first `opts.top`. `Item.free` is `pub` because `List.freeEach` calls it from the standard library.

```iris
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
```

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

```output
4.2 MiB  video.mp4
2.1 MiB  photos
40.0 KiB  code
12.0 KiB  notes.txt
3.0 KiB  [draft].md
demo: 6.4 MiB in 5 entries
```

## A table and colors

`term.table` lines the columns up. A column of numbers lines up on the right; sizes like `4.2 MiB` aren't numbers, so `t.align` puts that column on the right. Text for `term` can hold markup: `[blue]...[/]` colors the folders. A name from the disk could hold a `[`, so `term.escape` writes it as `[[` first.

`term.success` and `term.error` print a line after a green ✓ or a red ✗. Colors are only used on a terminal: in a pipe or a file, and when `NO_COLOR` is set, the markup is taken out.

```iris
import flags, fs, os, path, str, term
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

```output
    SIZE  NAME
 4.2 MiB  video.mp4
 2.1 MiB  photos/
40.0 KiB  code/
12.0 KiB  notes.txt
 3.0 KiB  [draft].md
✓ demo: 6.4 MiB in 5 entries
```

A folder that isn't there:

```output
✗ /nope: no such file
```

## A progress bar

Adding up a big folder takes a while. `term.progress` draws a bar on stderr, redrawn in place as `p.add()` is called. Lines printed with `term` while it runs go above it. `p.done()` writes the last line, with the time it took.

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

```output
demo  ██████████████████████████████  100%  6/6  0.0s
```

## Asking for folders

With `-i`, `sizes` asks for one folder after another. `term.readLine` shows a prompt and lets the line be edited with the usual keys: arrows, Home and End, Ctrl-W to cut a word, Up and Down for the lines typed before. `term.historyFile` keeps those lines in a file between runs. At Ctrl-D, `readLine` fails, which ends the loop.

```iris
@flags("shows what takes the space in a folder")
struct Options {
    @flag('n', "how many entries to show") i64 top = 10
    @flag('a', "count names that start with a dot") bool all
    @flag('i', "ask for folders, one after another") bool interactive
    @flag(args) str[] folders
}
```

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

```output
• type a folder to see what is in it; Ctrl-D ends
folder> demo/photos
demo/photos  ██████████████████████████████  100%  2/2  0.0s
     SIZE  NAME
  1.2 MiB  beach.jpg
900.0 KiB  hills.jpg
✓ demo/photos: 2.1 MiB in 2 entries
folder>
```

## The whole program

```iris
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
