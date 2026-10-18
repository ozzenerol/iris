# Build a TCP chat server

This guide builds `chat` from an empty folder: a server that people connect to over TCP, give a name, and then talk. Each line one of them types goes to everyone else. It uses the three ways Iris does several things at once. A task for each connection waits on that client and nothing else. Channels carry messages between the tasks. Threads accept connections on several CPU cores at the same time. Each step ends with a program that runs, and the commands to try it.

When it is finished, a session looks like this, as bob sees it:

```output
your name?
welcome, bob. 1 online
* alice joined
alice: hello bob
* alice left
online: bob. 2 connections so far
bye
```

## Before you start

You need two things:

- Iris. `iris version` prints its version.
- `nc` (netcat), to connect to the server. macOS has it. On Debian and Ubuntu, `sudo apt install netcat-openbsd` installs it. `command -v nc` prints where it is.

Make a folder for the server and go into it:

```shell
mkdir chat
cd chat
```

Everything in this guide happens in this folder. The program is one file, `main.iris`, which each step changes. You'll use two terminals or more: one runs the server, and the others connect to it.

## A task for each connection

The first version sends each line back to the client that sent it. It shows the shape every TCP server has: listen on a port, wait for a connection, and handle it.

Create `main.iris`. The `net` module has TCP:

```iris
// chat: a TCP chat server. Each line a client sends goes to everyone.
```

```iris
import net
```

`handle` serves one client. A `net.Conn` is one connection. `readLine` waits for the next line the client sends and gives it without its line break. The 1024 is a limit: a longer line fails with `NetError.tooLong`, so a client can't make the server keep an endless line. `readLine` also fails when the client goes away, which ends the loop. `write` sends text back. `defer c.close()` closes the connection however `handle` ends. A client that sends `/quit` gets `bye` and is let go.

The line `readLine` gives is a `String`, which owns its memory, so `handle` frees it once it has used it. `"{line}\n"` puts the line into text with a line break after it. Text with values given straight to a function is freed when the statement ends (see [Values in text](../tour/07-values-in-text.html)).

```iris
void handle(net.Conn c) {
    defer c.close()
    for {
        line := c.readLine(1024) catch e {
            return
        }
        if line == "/quit" {
            line.free()
            c.write("bye\n") catch e { }
            return
        }
        c.write("{line}\n") catch e {
            line.free()
            return
        }
        line.free()
    }
}
```

`main` listens on port 8080 with `net.listen`. `accept` waits for the next client and gives its connection. `spawn handle(conn)` starts `handle` as a new task and goes back to `accept` at once, without waiting for it. Each client has a task of its own, so one client that is slow to type doesn't hold up the others. While a task waits for its client, the other tasks run. The [Tasks and channels](../tour/24-tasks-and-channels.html) chapter explains how they take turns.

```iris
!void main() {
    ln := try net.listen(8080)
    print("listening on 8080")
    for {
        conn := try ln.accept()
        spawn handle(conn)
    }
}
```

```iris step
// chat: a TCP chat server. Each line a client sends goes to everyone.

import net

void handle(net.Conn c) {
    defer c.close()
    for {
        line := c.readLine(1024) catch e {
            return
        }
        if line == "/quit" {
            line.free()
            c.write("bye\n") catch e { }
            return
        }
        c.write("{line}\n") catch e {
            line.free()
            return
        }
        line.free()
    }
}

!void main() {
    ln := try net.listen(8080)
    print("listening on 8080")
    for {
        conn := try ln.accept()
        spawn handle(conn)
    }
}
```

Start the server in the first terminal. It keeps running, serving clients:

```shell start
iris run main.iris
```

In the second terminal, send it two lines and `/quit`. `printf` writes the lines, `|` passes them to `nc`, and `nc` sends them to the server and prints what comes back:

```shell try
printf 'hello\nagain\n/quit\n' | nc localhost 8080
```

```output
hello
again
bye
```

Each client has its own task, so a second client is served while the first is still connected. Here the first client connects, waits a second and then quits. The `&` at the end of its line lets it run in the background. The second client is answered in the meantime, and `cat` then shows what the first one got:

```shell try
{ echo first; sleep 1; echo /quit; } | nc localhost 8080 > first.txt &
sleep 0.2
printf 'second\n/quit\n' | nc localhost 8080
wait
cat first.txt
```

```output
second
bye
first
bye
```

To type lines yourself, run `nc localhost 8080` and type. Ctrl-C ends it.

Stop the server with Ctrl-C in the first terminal. Do this at the end of every step, and start it again with the new code at the next.

## A hub that passes the messages on

Now each line should go to every other client. But each client has its own task, and a task can't write to another client's connection: two tasks writing to one connection at the same time could mix their lines. So the server gets one task in charge of who is connected, the hub. The client tasks send it what happens, and it decides who gets which line. Nothing but the hub touches the list of clients, so the list needs no lock. Channels carry the messages both ways.

A channel carries values of one type from one task to another. `chan[T](n)` makes one with room for n values. `ch <- v` puts a value in, waiting while the channel is full, and `<-ch` takes the next one out, waiting while it is empty.

An `Event` is what a client's task tells the hub. It is an enum with data: each value carries what the hub needs. A client joins with its id, its name and the channel its lines go out on. It says a line, asks who is online, or leaves. The [Enums](../tour/15-enums.html) chapter has more on enums with data.

```iris
// What the clients' tasks tell the hub.
enum Event {
    Join(i64 id, String name, chan[String] out)
    Say(i64 id, String text)
    Who(i64 id)
    Leave(i64 id)
}
```

`Client` is a client as the hub sees it: its id, its name, and its channel. The hub sends a client its lines as `String`s on that channel.

```iris
// A client, as the hub sees it.
struct Client {
    i64 id
    String name
    chan[String] out
}
```

`broadcast` sends a line to every client except one, the one it came from. Each client gets its own `String` from `format`, since a `String` sent on a channel belongs to whoever takes it out. `find` gives the place of a client in the list. `farewell` sends a client its last line and then an empty `String`, which tells the client's writer to stop. Every real line ends with `\n`, so an empty one can't be mistaken for one.

```iris
// Sends text to every client but the one with id skip.
void broadcast(List[Client] *clients, str text, i64 skip) {
    for c in clients.items() {
        if c.id != skip {
            c.out <- format("{text}\n")
        }
    }
}
```

```iris
// The index of the client with id, or -1.
i64 find(List[Client] *clients, i64 id) {
    for i, c in clients.items() {
        if c.id == id {
            return i
        }
    }
    return -1
}
```

```iris
// Sends a client its last line, then the empty String that ends its writer.
void farewell(Client c, str text) {
    c.out <- format("{text}\n")
    c.out <- "".copy()
}
```

`hub` waits for the next event and does what it says. `match` takes the event apart: each arm names a kind of event and gives names to its data. A new client is told who is online, and the others are told it joined. A line goes to everyone else with the sender's name in front. `/who` lists the names to the client that asked. A client that leaves is taken out of the list, gets `bye`, and the others are told. The hub takes over each `String` it is sent, and frees it once it is done with it. A client's name is freed when the client leaves.

```iris
void hub(chan[Event] events) {
    List[Client] clients
    defer clients.free()
    for {
        e := <-events
        match e {
            Join(id, name, out) => {
                broadcast(&clients, "* {name} joined", id)
                c := Client{id: id, name: name, out: out}
                clients.push(c)
                out <- format("welcome, {name}. {clients.len} online\n")
            }
            Say(id, text) => {
                scoped String line = format("{clients.at(find(&clients, id)).name}: {text}")
                broadcast(&clients, line, id)
                text.free()
            }
            Who(id) => {
                String names
                for c in clients.items() {
                    names.append(" ")
                    names.append(c.name)
                }
                clients.at(find(&clients, id)).out <- format("online:{names}\n")
                names.free()
            }
            Leave(id) => {
                c := clients.remove(find(&clients, id))
                farewell(c, "bye")
                broadcast(&clients, "* {c.name} left", id)
                c.name.free()
            }
        }
    }
}
```

Each client now has two tasks. The task that reads its lines is `handle`. The one that writes to it is `writer`, which takes `String`s from the client's channel and writes them, until the empty one. If a write fails because the client has gone, it goes on taking lines without writing them, so the hub never waits on a full channel. When it stops, it sends `true` on `finished`.

```iris
void writer(net.Conn c, chan[String] out, chan[bool] finished) {
    bool failed
    for {
        msg := <-out
        if msg.len == 0 {
            msg.free()
            break
        }
        if !failed {
            c.write(msg) catch e {
                failed = true
            }
        }
        msg.free()
    }
    finished <- true
}
```

`handle` asks for a name, makes the client's channels, and starts its writer. It sends the hub `Join` and then sends each line, or `Who` for `/who`. At the end, after `/quit` or when the client goes away, it sends `Leave`. The hub then sends the writer its last line and the empty `String`. `handle` waits for `finished` before it closes the connection, so the writer is done with it first. The `String`s for the name and each line are sent to the hub, which frees them.

`finished` has room for one value, so the writer never waits for `handle` to take it. The client's channel has room for 64 lines, so the hub can pass on lines while a client is slow to read them.

```iris
void handle(net.Conn c, i64 id, chan[Event] events) {
    defer c.close()
    c.write("your name?\n") catch e {
        return
    }
    name := c.readLine(64) catch e {
        return
    }
    out := chan[String](64)
    finished := chan[bool](1)
    spawn writer(c, out, finished)
    events <- .Join(id, name, out)
    for {
        line := c.readLine(1024) catch e {
            break
        }
        if line == "/quit" {
            line.free()
            break
        }
        if line == "/who" {
            line.free()
            events <- .Who(id)
            continue
        }
        events <- .Say(id, line)
    }
    events <- .Leave(id)
    _ = <-finished
}
```

`main` makes the channel of events and starts the hub as a task. Then it accepts clients and gives each an id, counting from 1:

```iris
!void main() {
    ln := try net.listen(8080)
    events := chan[Event](256)
    spawn hub(events)
    print("listening on 8080")
    i64 id = 0
    for {
        conn := try ln.accept()
        id += 1
        spawn handle(conn, id, events)
    }
}
```

```iris step
// chat: a TCP chat server. Each line a client sends goes to everyone.

import net

// What the clients' tasks tell the hub.
enum Event {
    Join(i64 id, String name, chan[String] out)
    Say(i64 id, String text)
    Who(i64 id)
    Leave(i64 id)
}

// A client, as the hub sees it.
struct Client {
    i64 id
    String name
    chan[String] out
}

// Sends text to every client but the one with id skip.
void broadcast(List[Client] *clients, str text, i64 skip) {
    for c in clients.items() {
        if c.id != skip {
            c.out <- format("{text}\n")
        }
    }
}

// The index of the client with id, or -1.
i64 find(List[Client] *clients, i64 id) {
    for i, c in clients.items() {
        if c.id == id {
            return i
        }
    }
    return -1
}

// Sends a client its last line, then the empty String that ends its writer.
void farewell(Client c, str text) {
    c.out <- format("{text}\n")
    c.out <- "".copy()
}

void hub(chan[Event] events) {
    List[Client] clients
    defer clients.free()
    for {
        e := <-events
        match e {
            Join(id, name, out) => {
                broadcast(&clients, "* {name} joined", id)
                c := Client{id: id, name: name, out: out}
                clients.push(c)
                out <- format("welcome, {name}. {clients.len} online\n")
            }
            Say(id, text) => {
                scoped String line = format("{clients.at(find(&clients, id)).name}: {text}")
                broadcast(&clients, line, id)
                text.free()
            }
            Who(id) => {
                String names
                for c in clients.items() {
                    names.append(" ")
                    names.append(c.name)
                }
                clients.at(find(&clients, id)).out <- format("online:{names}\n")
                names.free()
            }
            Leave(id) => {
                c := clients.remove(find(&clients, id))
                farewell(c, "bye")
                broadcast(&clients, "* {c.name} left", id)
                c.name.free()
            }
        }
    }
}

void writer(net.Conn c, chan[String] out, chan[bool] finished) {
    bool failed
    for {
        msg := <-out
        if msg.len == 0 {
            msg.free()
            break
        }
        if !failed {
            c.write(msg) catch e {
                failed = true
            }
        }
        msg.free()
    }
    finished <- true
}

void handle(net.Conn c, i64 id, chan[Event] events) {
    defer c.close()
    c.write("your name?\n") catch e {
        return
    }
    name := c.readLine(64) catch e {
        return
    }
    out := chan[String](64)
    finished := chan[bool](1)
    spawn writer(c, out, finished)
    events <- .Join(id, name, out)
    for {
        line := c.readLine(1024) catch e {
            break
        }
        if line == "/quit" {
            line.free()
            break
        }
        if line == "/who" {
            line.free()
            events <- .Who(id)
            continue
        }
        events <- .Say(id, line)
    }
    events <- .Leave(id)
    _ = <-finished
}

!void main() {
    ln := try net.listen(8080)
    events := chan[Event](256)
    spawn hub(events)
    print("listening on 8080")
    i64 id = 0
    for {
        conn := try ln.accept()
        id += 1
        spawn handle(conn, id, events)
    }
}
```

Start the server:

```shell start
iris run main.iris
```

From the second terminal, two clients talk. bob connects first and waits. alice joins, says hello, and quits. bob then asks who is online, and quits too. bob's lines go to a file, which `cat` shows at the end:

```shell try
{ echo bob; sleep 1; echo /who; sleep 0.3; echo /quit; } | nc localhost 8080 > bob.txt &
sleep 0.3
printf 'alice\nhello bob\n/quit\n' | nc localhost 8080
wait
cat bob.txt
```

```output
your name?
welcome, alice. 2 online
bye
your name?
welcome, bob. 1 online
* alice joined
alice: hello bob
* alice left
online: bob
bye
```

alice's own line doesn't come back to her: `broadcast` skips the client a line came from. To chat for real, run `nc localhost 8080` in two or three terminals and type.

## Threads that accept

All the tasks so far run on one thread, on one CPU core. They take turns, so the server never does two things at the same moment. Now several threads accept connections, each with its own tasks, so clients are served on several cores at once. The [Threads](../tour/25-threads.html) chapter has the details.

Several threads can wait in `accept` on the same listener; each connection goes to one of them. The tasks a thread starts stay on that thread, so each client's `handle` and `writer` run on the thread that accepted it. Channels work between threads as they do between tasks, so the hub is unchanged.

The client ids now come from several threads at once. Two threads that add 1 to the same plain variable at the same moment can both read the old value, and give two clients the same id. An `Atomic` can be changed from any thread safely. `accepted.add(1)` adds 1 and gives the new value, which is the client's id and also the number of connections since the server started:

```iris
Atomic[i64] accepted
```

`acceptLoop` is the accept loop from `main`, in a function of its own so that each thread can run it. It takes its ids from `accepted`:

```iris
void acceptLoop(net.Listener ln, chan[Event] events) {
    for {
        conn := ln.accept() catch e {
            return
        }
        id := accepted.add(1)
        spawn handle(conn, id, events)
    }
}
```

`thread acceptLoop(ln, events)` starts a new thread running `acceptLoop`. `main` starts four, then runs the hub itself. The hub's thread never accepts, so it only passes messages on:

```iris
!void main() {
    ln := try net.listen(8080)
    events := chan[Event](256)
    for i in 0..4 {
        thread acceptLoop(ln, events)
    }
    print("listening on 8080")
    hub(events)
}
```

`/who` now says how many connections there have been. `accepted.load()` reads the count, from the hub's thread. This is the whole hub with that one line changed:

```iris
void hub(chan[Event] events) {
    List[Client] clients
    defer clients.free()
    for {
        e := <-events
        match e {
            Join(id, name, out) => {
                broadcast(&clients, "* {name} joined", id)
                c := Client{id: id, name: name, out: out}
                clients.push(c)
                out <- format("welcome, {name}. {clients.len} online\n")
            }
            Say(id, text) => {
                scoped String line = format("{clients.at(find(&clients, id)).name}: {text}")
                broadcast(&clients, line, id)
                text.free()
            }
            Who(id) => {
                String names
                for c in clients.items() {
                    names.append(" ")
                    names.append(c.name)
                }
                clients.at(find(&clients, id)).out <- format("online:{names}. {accepted.load()} connections so far\n")
                names.free()
            }
            Leave(id) => {
                c := clients.remove(find(&clients, id))
                farewell(c, "bye")
                broadcast(&clients, "* {c.name} left", id)
                c.name.free()
            }
        }
    }
}
```

```iris step
// chat: a TCP chat server. Each line a client sends goes to everyone.

import net

// What the clients' tasks tell the hub.
enum Event {
    Join(i64 id, String name, chan[String] out)
    Say(i64 id, String text)
    Who(i64 id)
    Leave(i64 id)
}

// A client, as the hub sees it.
struct Client {
    i64 id
    String name
    chan[String] out
}

Atomic[i64] accepted

// Sends text to every client but the one with id skip.
void broadcast(List[Client] *clients, str text, i64 skip) {
    for c in clients.items() {
        if c.id != skip {
            c.out <- format("{text}\n")
        }
    }
}

// The index of the client with id, or -1.
i64 find(List[Client] *clients, i64 id) {
    for i, c in clients.items() {
        if c.id == id {
            return i
        }
    }
    return -1
}

// Sends a client its last line, then the empty String that ends its writer.
void farewell(Client c, str text) {
    c.out <- format("{text}\n")
    c.out <- "".copy()
}

void hub(chan[Event] events) {
    List[Client] clients
    defer clients.free()
    for {
        e := <-events
        match e {
            Join(id, name, out) => {
                broadcast(&clients, "* {name} joined", id)
                c := Client{id: id, name: name, out: out}
                clients.push(c)
                out <- format("welcome, {name}. {clients.len} online\n")
            }
            Say(id, text) => {
                scoped String line = format("{clients.at(find(&clients, id)).name}: {text}")
                broadcast(&clients, line, id)
                text.free()
            }
            Who(id) => {
                String names
                for c in clients.items() {
                    names.append(" ")
                    names.append(c.name)
                }
                clients.at(find(&clients, id)).out <- format("online:{names}. {accepted.load()} connections so far\n")
                names.free()
            }
            Leave(id) => {
                c := clients.remove(find(&clients, id))
                farewell(c, "bye")
                broadcast(&clients, "* {c.name} left", id)
                c.name.free()
            }
        }
    }
}

void writer(net.Conn c, chan[String] out, chan[bool] finished) {
    bool failed
    for {
        msg := <-out
        if msg.len == 0 {
            msg.free()
            break
        }
        if !failed {
            c.write(msg) catch e {
                failed = true
            }
        }
        msg.free()
    }
    finished <- true
}

void handle(net.Conn c, i64 id, chan[Event] events) {
    defer c.close()
    c.write("your name?\n") catch e {
        return
    }
    name := c.readLine(64) catch e {
        return
    }
    out := chan[String](64)
    finished := chan[bool](1)
    spawn writer(c, out, finished)
    events <- .Join(id, name, out)
    for {
        line := c.readLine(1024) catch e {
            break
        }
        if line == "/quit" {
            line.free()
            break
        }
        if line == "/who" {
            line.free()
            events <- .Who(id)
            continue
        }
        events <- .Say(id, line)
    }
    events <- .Leave(id)
    _ = <-finished
}

void acceptLoop(net.Listener ln, chan[Event] events) {
    for {
        conn := ln.accept() catch e {
            return
        }
        id := accepted.add(1)
        spawn handle(conn, id, events)
    }
}

!void main() {
    ln := try net.listen(8080)
    events := chan[Event](256)
    for i in 0..4 {
        thread acceptLoop(ln, events)
    }
    print("listening on 8080")
    hub(events)
}
```

```shell start
iris run main.iris
```

The same two clients as before. bob's `/who` now also says how many connections the server has had. The count is of every connection since the server started, so it depends on how many you have made: 2 when these are the first.

```shell try
{ echo bob; sleep 1; echo /who; sleep 0.3; echo /quit; } | nc localhost 8080 > bob.txt &
sleep 0.3
printf 'alice\nhello bob\n/quit\n' | nc localhost 8080
wait
cat bob.txt
```

```output
your name?
welcome, alice. 2 online
bye
your name?
welcome, bob. 1 online
* alice joined
alice: hello bob
* alice left
online: bob. … connections so far
bye
```

## Stopping cleanly

So far Ctrl-C ends the server at once, and the clients' connections just drop. Now the server tells everyone it is stopping, waits for their writers to send that, and then ends.

The import line gets `signal`, which lets the program handle Ctrl-C itself:

```iris
import net, signal
```

The hub gets one more event, `Stop`:

```iris
// What the clients' tasks tell the hub.
enum Event {
    Join(i64 id, String name, chan[String] out)
    Say(i64 id, String text)
    Who(i64 id)
    Leave(i64 id)
    Stop
}
```

`signal.watch` stops Ctrl-C and the other stop signals from ending the program, and `signal.wait` waits for one. `stopOnSignal` waits in a task of its own and sends the hub `Stop` when a signal comes:

```iris
void stopOnSignal(chan[Event] events) {
    s := signal.wait()
    print("{s}: stopping")
    events <- .Stop
}
```

When `Stop` comes, the hub sends each client its last line, `the server is stopping`, and remembers that it is stopping. From then on a client that leaves gets no `bye`, and one that joins is told the server is stopping at once. When the last client has left, the hub sends `true` on `done` and returns:

```iris
void hub(chan[Event] events, chan[bool] done) {
    List[Client] clients
    defer clients.free()
    bool stopping
    for {
        e := <-events
        match e {
            Join(id, name, out) => {
                broadcast(&clients, "* {name} joined", id)
                c := Client{id: id, name: name, out: out}
                clients.push(c)
                if stopping {
                    farewell(c, "the server is stopping")
                } else {
                    out <- format("welcome, {name}. {clients.len} online\n")
                }
            }
            Say(id, text) => {
                scoped String line = format("{clients.at(find(&clients, id)).name}: {text}")
                broadcast(&clients, line, id)
                text.free()
            }
            Who(id) => {
                String names
                for c in clients.items() {
                    names.append(" ")
                    names.append(c.name)
                }
                clients.at(find(&clients, id)).out <- format("online:{names}. {accepted.load()} connections so far\n")
                names.free()
            }
            Leave(id) => {
                c := clients.remove(find(&clients, id))
                if !stopping {
                    farewell(c, "bye")
                    broadcast(&clients, "* {c.name} left", id)
                }
                c.name.free()
            }
            Stop => {
                stopping = true
                for c in clients.items() {
                    farewell(c, "the server is stopping")
                }
            }
        }
        if stopping && clients.len == 0 {
            done <- true
            return
        }
    }
}
```

A client that is stopped by the server is still waiting in `readLine` in its `handle` task. After its last line, `writer` calls `c.interrupt()`, which wakes it as if the client had gone away. `handle` then sends `Leave` and closes the connection as usual:

```iris
void writer(net.Conn c, chan[String] out, chan[bool] finished) {
    bool failed
    for {
        msg := <-out
        if msg.len == 0 {
            msg.free()
            break
        }
        if !failed {
            c.write(msg) catch e {
                failed = true
            }
        }
        msg.free()
    }
    c.interrupt()
    finished <- true
}
```

`main` starts the hub as a task again, and waits for it on `done`. That channel has no buffer, so the hub's `done <- true` waits until `main` takes it. When `main` returns, the program ends:

```iris
!void main() {
    ln := try net.listen(8080)
    events := chan[Event](256)
    done := chan[bool](0)
    for i in 0..4 {
        thread acceptLoop(ln, events)
    }
    signal.watch()
    spawn stopOnSignal(events)
    print("listening on 8080")
    spawn hub(events, done)
    _ = <-done
    print("stopped")
}
```

```iris step
// chat: a TCP chat server. Each line a client sends goes to everyone.

import net, signal

// What the clients' tasks tell the hub.
enum Event {
    Join(i64 id, String name, chan[String] out)
    Say(i64 id, String text)
    Who(i64 id)
    Leave(i64 id)
    Stop
}

// A client, as the hub sees it.
struct Client {
    i64 id
    String name
    chan[String] out
}

Atomic[i64] accepted

// Sends text to every client but the one with id skip.
void broadcast(List[Client] *clients, str text, i64 skip) {
    for c in clients.items() {
        if c.id != skip {
            c.out <- format("{text}\n")
        }
    }
}

// The index of the client with id, or -1.
i64 find(List[Client] *clients, i64 id) {
    for i, c in clients.items() {
        if c.id == id {
            return i
        }
    }
    return -1
}

// Sends a client its last line, then the empty String that ends its writer.
void farewell(Client c, str text) {
    c.out <- format("{text}\n")
    c.out <- "".copy()
}

void hub(chan[Event] events, chan[bool] done) {
    List[Client] clients
    defer clients.free()
    bool stopping
    for {
        e := <-events
        match e {
            Join(id, name, out) => {
                broadcast(&clients, "* {name} joined", id)
                c := Client{id: id, name: name, out: out}
                clients.push(c)
                if stopping {
                    farewell(c, "the server is stopping")
                } else {
                    out <- format("welcome, {name}. {clients.len} online\n")
                }
            }
            Say(id, text) => {
                scoped String line = format("{clients.at(find(&clients, id)).name}: {text}")
                broadcast(&clients, line, id)
                text.free()
            }
            Who(id) => {
                String names
                for c in clients.items() {
                    names.append(" ")
                    names.append(c.name)
                }
                clients.at(find(&clients, id)).out <- format("online:{names}. {accepted.load()} connections so far\n")
                names.free()
            }
            Leave(id) => {
                c := clients.remove(find(&clients, id))
                if !stopping {
                    farewell(c, "bye")
                    broadcast(&clients, "* {c.name} left", id)
                }
                c.name.free()
            }
            Stop => {
                stopping = true
                for c in clients.items() {
                    farewell(c, "the server is stopping")
                }
            }
        }
        if stopping && clients.len == 0 {
            done <- true
            return
        }
    }
}

void writer(net.Conn c, chan[String] out, chan[bool] finished) {
    bool failed
    for {
        msg := <-out
        if msg.len == 0 {
            msg.free()
            break
        }
        if !failed {
            c.write(msg) catch e {
                failed = true
            }
        }
        msg.free()
    }
    c.interrupt()
    finished <- true
}

void handle(net.Conn c, i64 id, chan[Event] events) {
    defer c.close()
    c.write("your name?\n") catch e {
        return
    }
    name := c.readLine(64) catch e {
        return
    }
    out := chan[String](64)
    finished := chan[bool](1)
    spawn writer(c, out, finished)
    events <- .Join(id, name, out)
    for {
        line := c.readLine(1024) catch e {
            break
        }
        if line == "/quit" {
            line.free()
            break
        }
        if line == "/who" {
            line.free()
            events <- .Who(id)
            continue
        }
        events <- .Say(id, line)
    }
    events <- .Leave(id)
    _ = <-finished
}

void acceptLoop(net.Listener ln, chan[Event] events) {
    for {
        conn := ln.accept() catch e {
            return
        }
        id := accepted.add(1)
        spawn handle(conn, id, events)
    }
}

void stopOnSignal(chan[Event] events) {
    s := signal.wait()
    print("{s}: stopping")
    events <- .Stop
}

!void main() {
    ln := try net.listen(8080)
    events := chan[Event](256)
    done := chan[bool](0)
    for i in 0..4 {
        thread acceptLoop(ln, events)
    }
    signal.watch()
    spawn stopOnSignal(events)
    print("listening on 8080")
    spawn hub(events, done)
    _ = <-done
    print("stopped")
}
```

This time, build the program with `iris build` and start what it makes, `./chat`. It has a name of its own, so the next command can find it:

```shell start
iris build main.iris -o chat
./chat
```

carol connects and waits. `pkill -INT -x chat` sends the server the signal Ctrl-C sends, so it is the same as pressing Ctrl-C in the first terminal:

```shell try
{ echo carol; sleep 2; } | nc localhost 8080 &
sleep 0.5
pkill -INT -x chat
wait
```

```output
your name?
welcome, carol. 1 online
the server is stopping
```

The server's terminal shows:

```output
listening on 8080
interrupt: stopping
stopped
```

A client that hasn't given its name yet hasn't joined, so the hub doesn't wait for it. Its connection closes when the program ends.

## The whole program

```iris whole
// chat: a TCP chat server. Each line a client sends goes to everyone.

import net, signal

// What the clients' tasks tell the hub.
enum Event {
    Join(i64 id, String name, chan[String] out)
    Say(i64 id, String text)
    Who(i64 id)
    Leave(i64 id)
    Stop
}

// A client, as the hub sees it.
struct Client {
    i64 id
    String name
    chan[String] out
}

Atomic[i64] accepted

// Sends text to every client but the one with id skip.
void broadcast(List[Client] *clients, str text, i64 skip) {
    for c in clients.items() {
        if c.id != skip {
            c.out <- format("{text}\n")
        }
    }
}

// The index of the client with id, or -1.
i64 find(List[Client] *clients, i64 id) {
    for i, c in clients.items() {
        if c.id == id {
            return i
        }
    }
    return -1
}

// Sends a client its last line, then the empty String that ends its writer.
void farewell(Client c, str text) {
    c.out <- format("{text}\n")
    c.out <- "".copy()
}

void hub(chan[Event] events, chan[bool] done) {
    List[Client] clients
    defer clients.free()
    bool stopping
    for {
        e := <-events
        match e {
            Join(id, name, out) => {
                broadcast(&clients, "* {name} joined", id)
                c := Client{id: id, name: name, out: out}
                clients.push(c)
                if stopping {
                    farewell(c, "the server is stopping")
                } else {
                    out <- format("welcome, {name}. {clients.len} online\n")
                }
            }
            Say(id, text) => {
                scoped String line = format("{clients.at(find(&clients, id)).name}: {text}")
                broadcast(&clients, line, id)
                text.free()
            }
            Who(id) => {
                String names
                for c in clients.items() {
                    names.append(" ")
                    names.append(c.name)
                }
                clients.at(find(&clients, id)).out <- format("online:{names}. {accepted.load()} connections so far\n")
                names.free()
            }
            Leave(id) => {
                c := clients.remove(find(&clients, id))
                if !stopping {
                    farewell(c, "bye")
                    broadcast(&clients, "* {c.name} left", id)
                }
                c.name.free()
            }
            Stop => {
                stopping = true
                for c in clients.items() {
                    farewell(c, "the server is stopping")
                }
            }
        }
        if stopping && clients.len == 0 {
            done <- true
            return
        }
    }
}

void writer(net.Conn c, chan[String] out, chan[bool] finished) {
    bool failed
    for {
        msg := <-out
        if msg.len == 0 {
            msg.free()
            break
        }
        if !failed {
            c.write(msg) catch e {
                failed = true
            }
        }
        msg.free()
    }
    c.interrupt()
    finished <- true
}

void handle(net.Conn c, i64 id, chan[Event] events) {
    defer c.close()
    c.write("your name?\n") catch e {
        return
    }
    name := c.readLine(64) catch e {
        return
    }
    out := chan[String](64)
    finished := chan[bool](1)
    spawn writer(c, out, finished)
    events <- .Join(id, name, out)
    for {
        line := c.readLine(1024) catch e {
            break
        }
        if line == "/quit" {
            line.free()
            break
        }
        if line == "/who" {
            line.free()
            events <- .Who(id)
            continue
        }
        events <- .Say(id, line)
    }
    events <- .Leave(id)
    _ = <-finished
}

void acceptLoop(net.Listener ln, chan[Event] events) {
    for {
        conn := ln.accept() catch e {
            return
        }
        id := accepted.add(1)
        spawn handle(conn, id, events)
    }
}

void stopOnSignal(chan[Event] events) {
    s := signal.wait()
    print("{s}: stopping")
    events <- .Stop
}

!void main() {
    ln := try net.listen(8080)
    events := chan[Event](256)
    done := chan[bool](0)
    for i in 0..4 {
        thread acceptLoop(ln, events)
    }
    signal.watch()
    spawn stopOnSignal(events)
    print("listening on 8080")
    spawn hub(events, done)
    _ = <-done
    print("stopped")
}
```

## Next

The server uses each way Iris has to do several things at once:

- A task for each connection, which waits on its client as if nothing else were happening.
- Channels between the tasks, with one task, the hub, in charge of what they share, so that it needs no lock.
- Threads that accept connections on several cores, with an `Atomic` for the count they share.

To run it for real, build it once with optimisations and start the program it makes:

```shell
iris build --mode=release-safe main.iris -o chat
./chat
```

Some ways to go on: `c.setTimeout(10m)` in `handle` lets a client go once it has said nothing for ten minutes, and `select` with an `after` arm lets the hub do something every so often. The [net library page](../std/index.html#net) has timeouts and TLS. The [Tasks and channels](../tour/24-tasks-and-channels.html) and [Threads](../tour/25-threads.html) chapters cover `select`, `Mutex` and `threadlocal`.
