# Input, output and logging

A program reads its input from standard input, and has two places to write text: standard output, for what it gives as its result, and standard error, for what it says about itself while it runs. `print` writes to standard output. The `io` module reads standard input and writes to either output, and the `log` module writes lines with the time and a level to standard error.

## Output and error

`io.write(text)` writes to standard output without a line break; `io.ewrite(text)` writes to standard error, and `io.eprint(text)` does the same with a line break:

```iris run
import io

void main() {
    io.write("HTTP/1.1 ")
    io.write("200 OK\n")
    io.eprint("this goes to standard error")
    print("Content-Length: 0")
}
```

```output
HTTP/1.1 200 OK
Content-Length: 0
```

The line on standard error isn't part of the output above: it shows in the terminal, but `./program > out.txt` puts only the other lines in the file, and `./program 2> errors.txt` puts it in its own file. Results go to standard output, so another program can read them; messages for the person running it go to standard error.

## Reading standard input

`io.readLine()` gives the next line of standard input as a new `String`, without its line break, and fails with `io.IoError.eof` when there is no more. `io.readAll()` gives all of it at once. A program that reads lines until the input ends, like a filter in a pipe, looks like this. It prints the client address at the start of each access log line:

```iris
import io, str

!void main() {
    i64 n = 0
    for {
        line := io.readLine() catch e {
            if e.code == io.IoError.eof {
                break
            }
            return e
        }
        n += 1
        ip, _, _ := str.cut(line, " ")
        print("{n}: {ip}")
        line.free()
    }
    io.eprint("{n} lines")
}
```

Given three lines in a pipe, it prints the addresses numbered, and the count on standard error:

```output
$ printf '10.0.0.5 GET /\n10.0.0.9 GET /login\n10.0.0.5 POST /login\n' | ./clients
1: 10.0.0.5
2: 10.0.0.9
3: 10.0.0.5
3 lines
```

When nothing is piped in, `io.readLine` waits for the person at the terminal to type a line; Ctrl-D ends the input. `term.readLine(prompt)` shows a prompt and lets the line be edited, with the lines typed before under Up and Down (see [Terminal output and input](07-terminal.html#reading-lines)).

## Log lines

`log.info(message)` writes a line to standard error, with the time in UTC and the level. `log.debug`, `log.warn` and `log.error` write at the other levels:

```iris run
import log

void main() {
    log.info("starting")
    port := 8080
    log.info("listening on {port}")
    log.warn("no config file, using the defaults")
    log.error("can't open the database")
    print("done")
}
```

```output
done
```

Only `done` is on standard output. The terminal shows the log lines as well:

```output
2026-10-17T09:30:00.250Z INFO starting
2026-10-17T09:30:00.250Z INFO listening on 8080
2026-10-17T09:30:00.250Z WARN no config file, using the defaults
2026-10-17T09:30:00.250Z ERROR can't open the database
```

The message is ordinary text, so values go in it with `{}` (see [Values in text](../tour/07-values-in-text.html)). Each line is written at once, so lines from several threads or tasks never mix.

## Fields

A line can carry values as fields after the message: a list of names, each with a `log.Value`, which is `.int`, `.float`, `.text` or `.bool`. They go after the message as `name=value`, and text with spaces is put in quotes. Fields are easier for a program to search than values written into the message:

```iris run
import log

void main() {
    log.showTime(false)
    peer := "10.0.0.5:51234"
    log.info("client connected", [("peer", .text(peer)), ("fd", .int(7)), ("tls", .bool(true))])
    log.warn("slow request", [("path", .text("/search")), ("ms", .int(950)), ("query", .text("status 502"))])
    print("done")
}
```

```output
done
```

Standard error gets:

```output
INFO client connected peer=10.0.0.5:51234 fd=7 tls=true
WARN slow request path=/search ms=950 query="status 502"
```

`log.showTime(false)` leaves the time out, as here. It suits a program that systemd or Docker runs, since they put the time on each line themselves.

## Levels

The levels are the values of `log.Level`, from least to most important:

| Level | For |
|---|---|
| `debug` | details for finding a problem |
| `info` | what the program is doing |
| `warn` | something went wrong, and the program goes on |
| `error` | something failed |

Lines below the level set with `log.setLevel` are left out. It starts at `info`, so `log.debug` writes nothing until the level is lowered. `log.write(level, message)` writes at a level held in a variable:

```iris run
import log

void main() {
    log.debug("not shown: the level starts at info")
    log.setLevel(.debug)
    log.debug("shown now")
    log.setLevel(.error)
    log.warn("not shown")
    level := log.Level.error
    log.write(level, "shown: error is not below error")
}
```

It prints nothing on standard output. Standard error gets two lines:

```output
2026-10-17T09:30:00.250Z DEBUG shown now
2026-10-17T09:30:00.250Z ERROR shown: error is not below error
```

A program usually sets the level once at the start, from a flag or an environment variable (see [Command-line flags](05-flags.html) and [Attributes](../tour/27-attributes.html)).

## JSON lines

`log.useJson()` writes each line as a JSON object, for programs that collect logs and search them. `log.useJson(false)` goes back to plain lines:

```iris run
import log

void main() {
    log.useJson()
    log.info("client \"edge-1\" connected")
    log.useJson(false)
    log.info("plain again")
}
```

Standard error gets:

```output
{"time": "2026-10-17T09:30:00.250Z", "level": "info", "message": "client \"edge-1\" connected"}
2026-10-17T09:30:00.250Z INFO plain again
```

The message is quoted with `json.quote`, so quotes and line breaks in it keep the line valid JSON. Fields become keys of the object, with numbers and `true` or `false` as JSON values: `"ms": 950`.

## Writing to a file

`log.toFile(path)` writes the lines at the end of a file instead, making it when it isn't there. It fails like `fs.openAppend` when the file can't be opened (see [Files](02-files.html)). `log.toStderr()` goes back to standard error and closes the file:

```iris run
import fs, log

!void main() {
    path := "/tmp/iris-tour-app.log"
    try fs.removeAll(path)
    log.showTime(false)
    try log.toFile(path)
    log.info("written to the file", [("n", .int(1))])
    log.toStderr()
    scoped String text = try fs.readAll(path)
    print(text)
    try fs.remove(path)
}
```

```output
INFO written to the file n=1

```
