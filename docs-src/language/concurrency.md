# Concurrency

Running several things at once: tasks for I/O, threads for CPU work, and channels to pass data between them.

## Tasks

`spawn` starts a function as a task and returns right away. Tasks are cheap, so a server can start one for each connection:

```iris
!void serve(u16 port) {
    ln := try net.listen(port)
    for {
        conn := try ln.accept()
        spawn handle(conn)
    }
}

void handle(Conn c) {
    defer c.close()
    for {
        line := c.readLine() catch e { return }
        _ = c.write(line)
    }
}
```

Tasks run on an event loop. When a task waits for the network, a file or a timer, another task runs. Code in a task is written as if it blocks, with no callbacks.

## Channels

A channel passes values from one task to another. It has a type and a size:

```iris
jobs := chan[Job](64)

spawn worker(jobs)
jobs <- Job{id: 1}
```

`<-c` takes the next value, waiting if there is none:

```iris
void worker(chan[Job] jobs) {
    for {
        j := <-jobs
        run(j)
    }
}
```

`select` waits on several channels at once and runs the arm of the first one ready. `after` gives a channel that receives once after a duration:

```iris
select {
    j := <-jobs => run(j)
    msg := <-control => handle(msg)
    after(5s) => print("idle")
}
```

## Threads

Tasks share threads. For work that keeps a CPU busy, start a thread:

```iris
t := thread.start(compress, data)
result := t.join()
```

`threadlocal` gives each thread its own copy of a global variable:

```iris
threadlocal i64 requests
```

## Atomics

An `atomic` variable can be changed safely by several threads at once. Each operation says how much ordering it needs:

```iris
atomic i64 hits

hits.add(1, .relaxed)
n := hits.load(.acquire)
```

## Locks

`sync.Mutex` protects data that several threads change:

```iris
sync.Mutex lock

lock.lock()
defer lock.unlock()
counts[key] += 1
```
