# Build a UDP counter server

This guide builds `counters` from an empty folder: a server that other programs tell what happened, like `hits:1` for a page that was shown or `errors:1` for a request that failed. It adds up the numbers, and says the totals when asked. Programs send it lines over UDP, where each message is one datagram that goes out at once, with no connection to open first and no answer to wait for. Each step ends with a program that runs, and the commands to try it.

When it is finished, using it looks like this:

```output
$ printf 'hits:1\nlogins:2\nhits:3\n' | nc -u -w1 127.0.0.1 8080
$ printf 'all' | nc -u -w1 127.0.0.1 8080
hits 4
logins 2
```

## Before you start

You need two things:

- Iris. `iris version` prints its version.
- `nc` (netcat), which sends and reads UDP datagrams from the shell. macOS has it. On Debian and Ubuntu, `sudo apt install netcat-openbsd` installs it. `command -v nc` prints where it is.

Make a folder for the server and go into it:

```shell
mkdir counters
cd counters
```

Everything in this guide happens in this folder. The program is one file, `main.iris`, which each step changes. You'll use two terminals: one runs the server, and the other sends it datagrams.

## Echoing datagrams

The first version sends every datagram back to whoever sent it. It shows the three calls a UDP server is made of: open a port, read a datagram, send one.

Create `main.iris`. The `net` module has UDP:

```iris
// counters: counts what other programs tell it over UDP.
```

```iris
import net
```

`net.listenUdp` opens UDP port 8080 and gives a `net.Udp`. `readFrom` waits for one datagram, puts its bytes in `buf`, and gives its length and who sent it, as a `net.Addr`: an IP address and a port. `from.format()` writes the address as text, like `127.0.0.1:36769`. `sendTo` sends one datagram to an address, here the bytes that came in, back to where they came from.

`buf` holds 1024 bytes. A datagram longer than that is cut to fit, which is plenty for lines like `hits:1`. While `readFrom` waits, other tasks of the program can run. The [Tasks and channels](../tour/24-tasks-and-channels.html) chapter explains how.

```iris
!void main() {
    sock := try net.listenUdp(8080)
    print("listening on 8080")
    u8[1024] buf
    for {
        n, from := try sock.readFrom(buf[0:1024])
        scoped String who = from.format()
        print("{n} bytes from {who}")
        try sock.sendTo(buf[0:n], from)
    }
}
```

```iris step
// counters: counts what other programs tell it over UDP.

import net

!void main() {
    sock := try net.listenUdp(8080)
    print("listening on 8080")
    u8[1024] buf
    for {
        n, from := try sock.readFrom(buf[0:1024])
        scoped String who = from.format()
        print("{n} bytes from {who}")
        try sock.sendTo(buf[0:n], from)
    }
}
```

Start the server in the first terminal. It keeps running, waiting for datagrams:

```shell start
iris run main.iris
```

In the second terminal, send it a datagram. `printf` writes the text with no newline after it, `nc -u` sends it over UDP and prints what comes back, and `-w1` makes `nc` stop after waiting a second for more:

```shell try
printf 'hello' | nc -u -w1 127.0.0.1 8080
```

```output
hello
```

The first terminal shows who sent it. The port is the one `nc` picked, so yours is different:

```output
listening on 8080
5 bytes from 127.0.0.1:36769
```

Stop the server with Ctrl-C in the first terminal. Do this at the end of every step, and start it again with the new code at the next.

## Counting

Now each datagram is a line like `hits:3`: a name, a colon and a number. The server adds the number to the counter of that name and sends back the new total.

A line that isn't a name and a number is an error. `CountError` is what it fails with:

```iris
enum CountError {
    badLine
}
```

The counters are a `Map[str, i64]`, from each name to its total. A map copies a `str` key, so the name stays after the datagram's bytes are gone. The [Collections](../tour/22-collections.html) chapter has maps.

`add` takes the map as a pointer, so it changes the server's map. `cut` splits the line at the first colon and says whether there was one. `parseInt` reads the number and fails on text that isn't one. `getOr` gives the counter's total, or 0 for a name that isn't there yet, and `set` stores the new one. The [Errors](../tour/18-errors.html) chapter explains `!T`, `try`, `catch` and `error`.

```iris
// Adds a line like hits:3 to its counter, and gives the new total.
!i64 add(Map[str, i64] *counts, str line) {
    name, num, found := line.cut(":")
    if !found || name.len == 0 {
        return error(CountError.badLine, "'{line}' isn't name:number")
    }
    n := num.parseInt() catch e {
        return error(CountError.badLine, "'{num}' isn't a number")
    }
    total := counts.getOr(name, 0) + n
    counts.set(name, total)
    return total
}
```

In `main`, the datagram's bytes become text: `buf[0:n]` is a slice of bytes, and a `str` can be set from one. `trim` takes off the newline a sender may put at the end. A bad line gets an answer that says what is wrong, and the server goes on with the next datagram. Text with values in it, passed straight to `sendTo`, needs no `format`. The [Values in text](../tour/07-values-in-text.html) chapter explains why.

```iris
!void main() {
    scoped Map[str, i64] counts
    sock := try net.listenUdp(8080)
    print("listening on 8080")
    u8[1024] buf
    for {
        n, from := try sock.readFrom(buf[0:1024])
        str text = buf[0:n]
        line := text.trim()
        total := add(&counts, line) catch e {
            try sock.sendTo("error: {e.message}\n", from)
            continue
        }
        name, _, _ := line.cut(":")
        try sock.sendTo("{name} {total}\n", from)
    }
}
```

```iris step
// counters: counts what other programs tell it over UDP.

import net

enum CountError {
    badLine
}

// Adds a line like hits:3 to its counter, and gives the new total.
!i64 add(Map[str, i64] *counts, str line) {
    name, num, found := line.cut(":")
    if !found || name.len == 0 {
        return error(CountError.badLine, "'{line}' isn't name:number")
    }
    n := num.parseInt() catch e {
        return error(CountError.badLine, "'{num}' isn't a number")
    }
    total := counts.getOr(name, 0) + n
    counts.set(name, total)
    return total
}

!void main() {
    scoped Map[str, i64] counts
    sock := try net.listenUdp(8080)
    print("listening on 8080")
    u8[1024] buf
    for {
        n, from := try sock.readFrom(buf[0:1024])
        str text = buf[0:n]
        line := text.trim()
        total := add(&counts, line) catch e {
            try sock.sendTo("error: {e.message}\n", from)
            continue
        }
        name, _, _ := line.cut(":")
        try sock.sendTo("{name} {total}\n", from)
    }
}
```

Start the server:

```shell start
iris run main.iris
```

Count some hits and a login, and see the totals come back:

```shell try
printf 'hits:1' | nc -u -w1 127.0.0.1 8080
printf 'hits:4' | nc -u -w1 127.0.0.1 8080
printf 'logins:2' | nc -u -w1 127.0.0.1 8080
```

```output
hits 1
hits 5
logins 2
```

Lines that aren't a name and a number get an error back:

```shell try
printf 'hello' | nc -u -w1 127.0.0.1 8080
printf 'hits:many' | nc -u -w1 127.0.0.1 8080
```

```output
error: 'hello' isn't name:number
error: 'many' isn't a number
```

## Many lines, and asking for totals

A program that counts many things sends many datagrams, and waiting for an answer to each one slows it down. Now a datagram can hold several lines, and counting sends nothing back. A sender sends and goes on. Two lines ask for totals instead: `get hits` sends back one counter, and `all` sends back every counter.

`add` no longer gives the total, since nobody gets it:

```iris
// Adds a line like hits:3 to its counter.
!void add(Map[str, i64] *counts, str line) {
    name, num, found := line.cut(":")
    if !found || name.len == 0 {
        return error(CountError.badLine, "'{line}' isn't name:number")
    }
    n := num.parseInt() catch e {
        return error(CountError.badLine, "'{num}' isn't a number")
    }
    counts.set(name, counts.getOr(name, 0) + n)
}
```

`report` writes every counter, one on each line. `keys` gives the map's names as a new list, in no particular order, and `sort` puts them in order with `before`, which says whether one name goes before another. `append` adds each line to the end of `out`. The caller gets `out` and frees it.

```iris
bool before(str a, str b) => a.compare(b) < 0
```

```iris
// Every counter, one on each line, sorted by name.
String report(Map[str, i64] *counts) {
    String out
    scoped List[str] names = counts.keys()
    names.sort(before)
    for name in names {
        scoped String line = format("{name} {counts.getOr(name, 0)}\n")
        out.append(line)
    }
    return out
}
```

`handle` takes one datagram apart into lines with `split`, and skips empty ones. `all` sends the report back, and `get` followed by a name sends that counter, 0 for one that isn't there. `line[4:line.len]` is the text after `get `. Any other line is counted. A bad line is printed on the server and dropped, and the rest of the datagram is still counted. Sending can fail, so `handle` can too.

```iris
// Handles one datagram. Each line adds to a counter, and only get and all
// send something back.
!void handle(net.Udp *sock, Map[str, i64] *counts, str msg, net.Addr from) {
    scoped List[str] lines = msg.split("\n")
    for raw in lines {
        line := raw.trim()
        if line.len == 0 {
            continue
        }
        if line == "all" {
            scoped String out = report(counts)
            try sock.sendTo(out, from)
        } else if line.startsWith("get ") {
            name := line[4:line.len].trim()
            try sock.sendTo("{name} {counts.getOr(name, 0)}\n", from)
        } else {
            add(counts, line) catch e {
                print("dropped a line: {e.message}")
            }
        }
    }
}
```

`main` gives each datagram to `handle`. A datagram that couldn't be answered is printed, and the server goes on with the next.

```iris
!void main() {
    scoped Map[str, i64] counts
    sock := try net.listenUdp(8080)
    print("listening on 8080")
    u8[1024] buf
    for {
        n, from := try sock.readFrom(buf[0:1024])
        handle(&sock, &counts, buf[0:n], from) catch e {
            print("couldn't answer: {e.message}")
        }
    }
}
```

```iris step
// counters: counts what other programs tell it over UDP.

import net

enum CountError {
    badLine
}

// Adds a line like hits:3 to its counter.
!void add(Map[str, i64] *counts, str line) {
    name, num, found := line.cut(":")
    if !found || name.len == 0 {
        return error(CountError.badLine, "'{line}' isn't name:number")
    }
    n := num.parseInt() catch e {
        return error(CountError.badLine, "'{num}' isn't a number")
    }
    counts.set(name, counts.getOr(name, 0) + n)
}

bool before(str a, str b) => a.compare(b) < 0

// Every counter, one on each line, sorted by name.
String report(Map[str, i64] *counts) {
    String out
    scoped List[str] names = counts.keys()
    names.sort(before)
    for name in names {
        scoped String line = format("{name} {counts.getOr(name, 0)}\n")
        out.append(line)
    }
    return out
}

// Handles one datagram. Each line adds to a counter, and only get and all
// send something back.
!void handle(net.Udp *sock, Map[str, i64] *counts, str msg, net.Addr from) {
    scoped List[str] lines = msg.split("\n")
    for raw in lines {
        line := raw.trim()
        if line.len == 0 {
            continue
        }
        if line == "all" {
            scoped String out = report(counts)
            try sock.sendTo(out, from)
        } else if line.startsWith("get ") {
            name := line[4:line.len].trim()
            try sock.sendTo("{name} {counts.getOr(name, 0)}\n", from)
        } else {
            add(counts, line) catch e {
                print("dropped a line: {e.message}")
            }
        }
    }
}

!void main() {
    scoped Map[str, i64] counts
    sock := try net.listenUdp(8080)
    print("listening on 8080")
    u8[1024] buf
    for {
        n, from := try sock.readFrom(buf[0:1024])
        handle(&sock, &counts, buf[0:n], from) catch e {
            print("couldn't answer: {e.message}")
        }
    }
}
```

Start the server:

```shell start
iris run main.iris
```

Send three lines in one datagram, then a bad line and a good one in another. `\n` in the `printf` text is a newline. Nothing comes back:

```shell try
printf 'hits:1\nlogins:2\nhits:3\n' | nc -u -w1 127.0.0.1 8080
printf 'oops\nerrors:1' | nc -u -w1 127.0.0.1 8080
```

The first terminal prints `dropped a line: 'oops' isn't name:number`, and `errors:1` from the same datagram is counted.

Ask for one counter, one that isn't there, and all of them:

```shell try
printf 'get hits' | nc -u -w1 127.0.0.1 8080
printf 'get misses' | nc -u -w1 127.0.0.1 8080
printf 'all' | nc -u -w1 127.0.0.1 8080
```

```output
hits 4
misses 0
errors 1
hits 4
logins 2
```

## The whole program

```iris whole
// counters: counts what other programs tell it over UDP.

import net

enum CountError {
    badLine
}

// Adds a line like hits:3 to its counter.
!void add(Map[str, i64] *counts, str line) {
    name, num, found := line.cut(":")
    if !found || name.len == 0 {
        return error(CountError.badLine, "'{line}' isn't name:number")
    }
    n := num.parseInt() catch e {
        return error(CountError.badLine, "'{num}' isn't a number")
    }
    counts.set(name, counts.getOr(name, 0) + n)
}

bool before(str a, str b) => a.compare(b) < 0

// Every counter, one on each line, sorted by name.
String report(Map[str, i64] *counts) {
    String out
    scoped List[str] names = counts.keys()
    names.sort(before)
    for name in names {
        scoped String line = format("{name} {counts.getOr(name, 0)}\n")
        out.append(line)
    }
    return out
}

// Handles one datagram. Each line adds to a counter, and only get and all
// send something back.
!void handle(net.Udp *sock, Map[str, i64] *counts, str msg, net.Addr from) {
    scoped List[str] lines = msg.split("\n")
    for raw in lines {
        line := raw.trim()
        if line.len == 0 {
            continue
        }
        if line == "all" {
            scoped String out = report(counts)
            try sock.sendTo(out, from)
        } else if line.startsWith("get ") {
            name := line[4:line.len].trim()
            try sock.sendTo("{name} {counts.getOr(name, 0)}\n", from)
        } else {
            add(counts, line) catch e {
                print("dropped a line: {e.message}")
            }
        }
    }
}

!void main() {
    scoped Map[str, i64] counts
    sock := try net.listenUdp(8080)
    print("listening on 8080")
    u8[1024] buf
    for {
        n, from := try sock.readFrom(buf[0:1024])
        handle(&sock, &counts, buf[0:n], from) catch e {
            print("couldn't answer: {e.message}")
        }
    }
}
```

## Next

The server is made of three calls from the `net` module:

- `net.listenUdp` opens a UDP port.
- `readFrom` waits for one datagram and says who sent it.
- `sendTo` sends one datagram to an address.

UDP doesn't promise that a datagram arrives, or that datagrams arrive in the order they were sent. That suits counters: a lost `hits:1` makes one total a little low, and the sender never waits.

To run it for real, build it once with optimisations and start the program it makes:

```shell
iris build --mode=release-safe main.iris -o counters
./counters
```

Some ways to go on: another Iris program can send counts with `net.dialUdp`, which gives a socket that `send`s to one address, and `setTimeout` so a `read` of the answer to `get` stops waiting when the datagram is lost. A gauge, a line like `queue=12` that sets a value instead of adding to it, takes one more case in `handle`. The totals could be written to a file every minute by a task that sleeps between writes. The [net library page](../std/index.html#net) has the rest of UDP, and the [Build a DNS server](04-dns-server.html) guide is a bigger UDP server.
