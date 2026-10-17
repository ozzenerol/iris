## Colors as markup

`term.print` writes a line with its markup made into colors when standard output is a terminal. Sent to a pipe or a file, the same program writes the text without the markup, so its output can be read by other programs:

```iris run
import term

void main() {
    name := "report.pdf"
    term.print("[green]ok[/] saved [bold]{name}[/]")
    term.print("[yellow on blue] note [/] [[brackets] stay")
    scoped String s = term.plain("[red]3 failed[/]")
    print("{s}, {term.width("日本")} columns wide")
}
```

```output
ok saved report.pdf
 note  [brackets] stay
3 failed, 4 columns wide
```

`[[` writes a `[`. Text from users can hold brackets that look like markup, so it goes through `term.escape` first: `term.print("hello [bold]{term.escape(name)}[/]")`.

## Status lines

`success` and `info` write a line with a sign in front on standard output, and `warn` and `error` on standard error. `title` writes a bold line to start a part of the output:

```iris
term.title("Deploy")
term.info("3 services to update")
term.success("web is up")
term.warn("db took 12s")
term.error("cache didn't answer")
```

## Tables

`term.table` takes the headers, `row` adds a row, and `print` writes the table with each column as wide as its widest cell. Columns of numbers line up on the right; `align` sets a column's alignment and `border` its frame (`.none`, `.box` or `.markdown`):

```iris run
import term

void main() {
    scoped t := term.table(["NAME", "STATUS", "CPU"])
    t.row(["web", "[green]running[/]", "12.5"])
    t.row(["db", "[red]stopped[/]", "3"])
    t.print()
    t.border = .box
    t.align(1, .center)
    t.print()
}
```

```output
NAME  STATUS    CPU
web   running  12.5
db    stopped     3
╭──────┬─────────┬──────╮
│ NAME │ STATUS  │  CPU │
├──────┼─────────┼──────┤
│ web  │ running │ 12.5 │
│ db   │ stopped │    3 │
╰──────┴─────────┴──────╯
```

## Progress

`term.progress` shows a bar on standard error that `add` or `set` moves, and `done` ends. Lines written with term's functions while it runs go above it. When standard error isn't a terminal, only the last line is written:

```iris
bar := term.progress("downloading", files.len)
for f in files {
    fetch(f)
    bar.add()
}
bar.done()
```

## Reading lines

`term.readLine` shows a prompt and reads a line, with editing and a history: Left and Right move in the line, Up and Down go through the lines before, and Ctrl-A, Ctrl-E, Ctrl-U, Ctrl-K and Ctrl-W work as in a shell. `historyFile` keeps the history between runs. At the end of input, or on Ctrl-D, it fails with `IoError.eof`:

```iris
import term

void main() {
    term.historyFile(".notes_history")
    for {
        line := term.readLine("> ") catch e {
            break
        }
        print("you wrote {line}")
        line.free()
    }
}
```

`readPassword` reads a line without showing it. When standard input isn't a terminal, both read plain lines.
