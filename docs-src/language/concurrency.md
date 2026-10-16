# Concurrency

Running several things at once: tasks that take turns while they wait, and channels to pass values between them.

## Tasks

`spawn` starts a function call as a task and returns right away. The arguments are worked out first. Tasks are cheap, so a server can start one for each connection:

```iris
!void serve(u16 port) {
    ln := try net.listen(port)
    for {
        conn := try ln.accept()
        spawn handle(conn)
    }
}

void handle(net.Conn c) {
    defer c.close()
    for {
        scoped String line = c.readLine() catch e { return }
        c.write(line) catch e { return }
    }
}
```

Each task has its own stack. All tasks run on one thread and take turns: when a task waits for the network, a channel or a timer, the next ready task runs. Code in a task is written as if it blocks, with no callbacks.

A task can't return an error, since nothing waits for its result: a spawned function returns no `!T`. The program ends when `main` returns, even if other tasks are still running.

`sleep(d)` waits for a `Duration` and lets other tasks run meanwhile.

## Channels

A channel passes values from one task to another. It has a type and room for a number of values, at least one:

```iris
jobs := chan[Job](64)

spawn worker(jobs)
jobs <- Job{id: 1}
```

`ch <- v` puts a value in, waiting while the channel is full. `<-ch` takes the next value out, waiting while it is empty:

```iris
void worker(chan[Job] jobs) {
    for {
        j := <-jobs
        run(j)
    }
}
```

`select` waits on several channels at once and runs the arm of the first one with a value. An `after(d)` arm runs when nothing came within that time:

```iris
select {
    j := <-jobs => run(j)
    msg := <-control => handle(msg)
    after(5s) => print("idle")
}
```

When every task is waiting and nothing can wake any of them, the program stops with a panic that says so.

A complete program:

```iris run
void square(i64 n, chan[i64] out) {
    sleep(1ms)
    out <- n * n
}

void main() {
    results := chan[i64](8)
    for i in 1..4 {
        spawn square(i, results)
    }
    i64 total
    for _ in 1..4 {
        total += <-results
    }
    print("total {total}")

    quiet := chan[str](1)
    select {
        msg := <-quiet => print(msg)
        after(10ms) => print("nothing came")
    }
}
```

```output
total 14
nothing came
```

## Threads

Threads, `threadlocal`, atomics and locks are planned for a later version. Until then all tasks run on the program's one thread, which is what the network library needs.
