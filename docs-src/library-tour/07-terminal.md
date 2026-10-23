# Terminal output and input

A command-line program talks to a person at a terminal, and often to other programs through a pipe as well. The `term` module writes colors, status lines, tables and progress bars when a person is watching, and plain text when the output goes to a pipe or a file. It also reads lines with editing and a history, and passwords without showing them.

## Colors

`term.print` writes a line with color markup in it. A tag is a list of words in brackets: a color (`black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`, `gray`), `on` and a color for the background, and `bold`, `dim`, `italic`, `underline`, `reverse` or `strike`. `[/]` closes the last tag:

```iris run
import term

void main() {
    term.print("[green]PASS[/] parser   [dim](12 tests)[/]")
    term.print("[red bold]FAIL[/] runtime  [dim](1 of 40)[/]")
    term.print("[black on yellow] SLOW [/] 2.4s in [underline]tests/net.iris[/]")
}
```

```output
PASS parser   (12 tests)
FAIL runtime  (1 of 40)
 SLOW  2.4s in tests/net.iris
```

Colors are on when the output is a terminal. Sent to a pipe or a file, as in the output above, the markup is taken out and the text stays, so other programs read plain lines. `NO_COLOR` set to anything turns colors off, and `FORCE_COLOR` turns them on, as many tools agree. `term.colors()` says whether they are on, and `term.setColors` decides for the program.

`[[` writes a `[`, and brackets around anything that isn't a tag stay as they are. Text from outside the program, such as a file name, may hold something that looks like a tag, so it goes through `term.escape` first:

```iris run
import term

void main() {
    name := "[red]notes.txt"
    scoped String safe = term.escape(name)
    term.print("saved [bold]{safe}[/] [[1 of 3]")
    scoped String text = term.plain("[green]ok[/] done")
    print("{text}, {text.len} bytes")
}
```

```output
saved [red]notes.txt [1 of 3]
ok done, 7 bytes
```

`term.plain` gives the text without its markup, and `term.format` gives it with colors as a `String`, for text that is written later or elsewhere. `term.eprint` and `term.write` are `print` for standard error and without a line break.

## Status lines

`success` and `info` write a line with a sign in front of it on standard output, and `warn` and `error` on standard error, where messages belong. `title` writes a bold line that starts a part of the output:

```iris
import term

void main() {
    term.title("Deploying 3 services")
    term.success("api is up, version 1.4.2")
    term.info("worker is draining its queue")
    term.warn("db took 12s to answer")
    term.error("cache didn't start: port 6379 is taken")
}
```

```output
Deploying 3 services
✓ api is up, version 1.4.2
• worker is draining its queue
! db took 12s to answer
✗ cache didn't start: port 6379 is taken
```

On a terminal the signs are green, blue, yellow and red.

## Tables

`term.table(headers)` starts a table, `row` adds a row, and `print` writes it, with each column as wide as its widest cell. A column of numbers lines up on the right, and any other on the left. Cells can have markup, which doesn't count toward the width:

```iris run
import term

void main() {
    scoped t := term.table(["SERVICE", "STATE", "CPU %", "MEMORY"])
    t.row(["api", "[green]running[/]", "12.5", "180 MiB"])
    t.row(["worker", "[green]running[/]", "71", "1.2 GiB"])
    t.row(["cron", "[red]stopped[/]", "0", "-"])
    t.print()
    print("{t.len} services")
}
```

```output
SERVICE  STATE    CPU %  MEMORY
api      running   12.5  180 MiB
worker   running     71  1.2 GiB
cron     stopped      0  -
3 services
```

`align(col, a)` sets how a column lines up, `.left`, `.right` or `.center`, and `border` draws a frame: `.box` with lines, or `.markdown` for a table to paste into a README or an issue:

```iris run
import term

void main() {
    scoped t := term.table(["Benchmark", "Time", "Memory"])
    t.row(["json decode", "41 ms", "12 MiB"])
    t.row(["http hello", "9 ms", "3 MiB"])
    t.align(1, .right)
    t.align(2, .right)
    t.border = .markdown
    t.print()
    t.border = .box
    t.print()
}
```

```output
| Benchmark   |  Time | Memory |
| ----------- | ----: | -----: |
| json decode | 41 ms | 12 MiB |
| http hello  |  9 ms |  3 MiB |
╭─────────────┬───────┬────────╮
│ Benchmark   │  Time │ Memory │
├─────────────┼───────┼────────┤
│ json decode │ 41 ms │ 12 MiB │
│ http hello  │  9 ms │  3 MiB │
╰─────────────┴───────┴────────╯
```

`t.format()` gives the table as a `String` instead of writing it.

## Progress

`term.progress(label, total)` shows a bar on standard error for work done in steps. `add` moves it one step, or a number of steps, `set` moves it to a count, and `done` draws the last line with the time it took. While the bar is shown, lines written with `term` go above it:

```iris
import term

void main() {
    files := ["a.log", "b.log", "c.log"]
    bar := term.progress("compressing", files.len)
    for f in files {
        compress(f)
        term.print("[dim]{f}[/]")
        bar.add()
    }
    bar.done()
}
```

```output
a.log
b.log
c.log
compressing  ██████████████████████████████  100%  3/3  0.4s
```

The bar is drawn again only when its percent changes, or every 100 ms, so moving it in a tight loop costs little. When standard error isn't a terminal, only the last line is written, so a log file doesn't fill with bars.

## The terminal's size

`term.size()` gives the terminal's width and height in characters, and 80 by 24 when the output isn't a terminal. `term.width(text)` gives how many columns text takes, which differs from its length in bytes for letters outside ASCII and for wide characters, and leaves out markup:

```iris run
import term

void main() {
    cols, rows := term.size()
    print("{cols}x{rows}")
    print("{term.width("café")} columns, {"café".len} bytes")
    print("{term.width("日本語")} columns, {term.width("[bold]ok[/]")} for the markup")
    print("interactive: {term.interactive()}")
}
```

```output
80x24
4 columns, 5 bytes
6 columns, 2 for the markup
interactive: false
```

`term.interactive()` is true when standard input and output are both a terminal, which is when it makes sense to ask the person a question.

## Reading lines

`term.readLine(prompt)` shows a prompt and reads a line that the person can edit: Left and Right move in the line, Up and Down bring back earlier lines, and Ctrl-A, Ctrl-E, Ctrl-U, Ctrl-K and Ctrl-W work as they do in a shell. `historyFile(path)` keeps the earlier lines between runs. At the end of the input, or on Ctrl-D, it fails with `IoError.eof`:

```iris
import term

void main() {
    term.historyFile("/tmp/calc_history")
    for {
        line := term.readLine("[cyan]calc>[/] ") catch e {
            break
        }
        defer line.free()
        if line == "quit" {
            break
        }
        n := line.parseInt() catch e {
            term.error("{line} isn't a number")
            continue
        }
        term.print("[bold]{n * n}[/]")
    }
}
```

A session looks like this:

```output
calc> 12
144
calc> twelve
✗ twelve isn't a number
calc> quit
```

`term.readPassword(prompt)` reads a line without showing it, and leaves it out of the history. When standard input isn't a terminal, both read plain lines, so the same program takes its input from a pipe:

```iris
import term

!void main() {
    user := try term.readLine("user: ")
    defer user.free()
    pass := try term.readPassword("password: ")
    defer pass.free()
    term.success("signing in as {user}, with a password of {pass.len} characters")
}
```

`term.clear()` clears the screen.
