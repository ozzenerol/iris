# Tasks and channels

A task is a function call that runs alongside the rest of the program. Tasks take turns on a thread, switching whenever one waits, so a program can do many things at once without callbacks. Channels pass values between them. This chapter covers `spawn`, channels, `select` and timers.

## spawn

`spawn` starts a function call as a new task and goes on at once, without waiting for it. Its arguments are worked out first, in the task that spawns:

```iris run
void fetch(str host, Duration rtt) {
    sleep(rtt) // the time the reply takes to come back
    print("reply from {host}")
}

void main() {
    spawn fetch("eu.example.com", 20ms)
    spawn fetch("us.example.com", 5ms)
    print("both requests sent")
    sleep(40ms)
    print("done")
}
```

```output
both requests sent
reply from us.example.com
reply from eu.example.com
done
```

`sleep(d)` waits for a `Duration`, and while a task waits, the others run. The same is true of every wait: for the network, a file, a channel, a lock, a timer. Code in a task is written in order, as if each call simply took its time, and the waiting is where the tasks take turns.

Tasks are cheap: each has its own stack, kept small until it is needed, so a server can start one for each connection. The program ends when `main` returns, whether or not other tasks are still running.

A spawned function can't return an error, since nothing waits for its result. It handles its errors itself, or sends them to a task that does.

## Channels

A channel carries values of one type from one task to another. `chan[T](n)` makes one with room for n values. `ch <- v` puts a value in, waiting while the channel is full; `<-ch` takes the next value out, waiting while it is empty.

Here a server task reads the lines of a connection and passes each one to `main` on a channel. `net.listen(0, "127.0.0.1")` listens on a free port, which `ln.port` then holds, and `net.dial` connects to it (see [the net library](../std/index.html#net)):

```iris run
import net

// Reads the lines of one connection and passes each one on.
void serve(net.Listener ln, chan[String] lines) {
    conn := ln.accept() catch e {
        return
    }
    defer conn.close()
    for {
        line := conn.readLine(1024) catch e {
            return
        }
        lines <- line
    }
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    lines := chan[String](8)
    spawn serve(ln, lines)
    c := try net.dial("127.0.0.1", ln.port)
    defer c.close()
    try c.write("PING\nSTATS\nQUIT\n")
    for i in 0..3 {
        scoped String l = <-lines
        print("the server read {l}")
    }
}
```

```output
the server read PING
the server read STATS
the server read QUIT
```

Values come out in the order they went in. A channel is a value that refers to the same queue wherever it is copied, so it can be passed to tasks as an argument, kept in a struct or held in a global. Several tasks can send to one channel, and several can take from it.

A value sent on a channel is copied into it. A `String` or another value that owns memory is handed over that way: the task that takes it frees it. Above, `serve` sends each line it read, and `main` frees it.

## Freeing a channel

`ch.free()` gives back a channel's memory. Free it once no task uses it anymore. Values still in it are not freed, so take out any that own memory first. Freeing a channel that a task is waiting on stops the program with a panic. A channel that holds no queue yet (a `chan[T]` variable never given one) frees as nothing. `scoped` works too:

```iris run
void main() {
    for i in 0..3 {
        scoped chan[i64] ch = chan[i64](1)
        ch <- i * 10
        print("took {<-ch}")
    }
}
```

```output
took 0
took 10
took 20
```

## No buffer

A channel made with room for 0 values has no buffer: `ch <- v` waits until another task takes the value, which goes straight from one to the other. Both sides know the hand-over happened, which makes it a way for tasks to wait for each other. Here `main` waits until the server is listening before it connects:

```iris run
import net

// Listens, says it is ready by sending its port, then greets one client.
void serve(chan[u16] ready) {
    ln := net.listen(0, "127.0.0.1") catch e {
        ready <- 0
        return
    }
    ready <- ln.port
    conn := ln.accept() catch e {
        return
    }
    defer conn.close()
    conn.write("220 ready\n") catch e { }
}

!void main() {
    ready := chan[u16](0)
    spawn serve(ready)
    port := <-ready
    if port == 0 {
        print("the server didn't start")
        return
    }
    c := try net.dial("127.0.0.1", port)
    defer c.close()
    scoped String hello = try c.readLine()
    print("connected: {hello}")
}
```

```output
connected: 220 ready
```

## Workers

A common shape: a channel of jobs, a few tasks taking jobs from it, and a channel for the results. Here three workers check which ports accept a connection. Ports 1 to 3 have nothing listening on them, so connecting fails at once:

```iris run
import net

struct Job {
    i64 index
    u16 port
}

struct Result {
    i64 index
    bool open
}

void worker(chan[Job] jobs, chan[Result] results) {
    for {
        j := <-jobs
        c := net.dial("127.0.0.1", j.port, 1s) catch e {
            results <- Result{index: j.index}
            continue
        }
        c.close()
        results <- Result{index: j.index, open: true}
    }
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    defer ln.close()
    u16[4] ports = [ln.port, 1, 2, 3]
    jobs := chan[Job](16)
    results := chan[Result](16)
    for w in 0..3 {
        spawn worker(jobs, results)
    }
    for i, p in ports {
        jobs <- Job{index: i, port: p}
    }
    bool[4] open
    for i in 0..ports.len {
        r := <-results
        open[r.index] = r.open
    }
    print("our listener: {open[0]}; ports 1 to 3: {open[1]} {open[2]} {open[3]}")
}
```

```output
our listener: true; ports 1 to 3: false false false
```

The results come back in whatever order the checks finish, so each carries the index of its job. The workers loop forever, taking jobs as they come; when `main` has all the results it returns, and the program ends.

## select

`select` waits on several channels at once and runs the arm of the first one that has a value. An `after(d)` arm runs when nothing came in that time, which gives any wait a timeout. Here the server accepts the connection but never answers, so the reply never comes:

```iris run
import net

// Asks a server for a line and passes the reply on.
void ask(u16 port, chan[String] replies) {
    c := net.dial("127.0.0.1", port) catch e {
        return
    }
    defer c.close()
    c.write("PING\n") catch e {
        return
    }
    line := c.readLine() catch e {
        return
    }
    replies <- line
}

!void main() {
    ln := try net.listen(0, "127.0.0.1") // nothing here ever answers
    defer ln.close()
    replies := chan[String](1)
    control := chan[str](1)
    spawn ask(ln.port, replies)
    control <- "reload"
    select {
        r := <-replies => print(r)
        c := <-control => print("control: {c}")
        after(1s) => print("nothing came")
    }
    select {
        r := <-replies => print(r)
        after(50ms) => print("no reply within 50ms")
    }
}
```

```output
control: reload
no reply within 50ms
```

When several channels have values, `select` takes the first arm in order. An arm takes from a channel, or waits with `after`; sending is done outside `select`.

## Deadlocks

When every task waits for something that no other task can give, the program can never go on. It stops with a panic that says so, instead of hanging:

```iris
void main() {
    replies := chan[String](0)
    r := <-replies // no task will ever send
}
```

```output
panic: every task is waiting and nothing can wake them: a deadlock
```

## Errors from tasks

A task that can fail sends its error, or its result, on a channel. A struct carries either. Here each probe sends what it found, with the error's code when connecting failed:

```iris run
import net

struct Probe {
    i64 index
    bool ok
    String why
}

void probe(i64 index, u16 port, chan[Probe] out) {
    c := net.dial("127.0.0.1", port, 1s) catch e {
        out <- Probe{index: index, why: format("{e.code}")}
        return
    }
    c.close()
    out <- Probe{index: index, ok: true}
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    defer ln.close()
    out := chan[Probe](4)
    spawn probe(0, ln.port, out)
    spawn probe(1, 1, out)
    str[2] names = ["our listener", "port 1"]
    Probe[2] found
    for i in 0..2 {
        p := <-out
        found[p.index] = p
    }
    for p in found {
        if p.ok {
            print("{names[p.index]}: open")
        } else {
            print("{names[p.index]}: {p.why}")
        }
        p.why.free()
    }
}
```

```output
our listener: open
port 1: NetError.refused
```

Tasks on one thread never run at the same moment; the next chapter covers threads, which do.
