# Concurrency

Running several things at once: tasks that take turns while they wait, threads that run at the same time, and channels, atomics and locks to share work between them.

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

Each task has its own stack. The tasks of a thread take turns: when a task waits for the network, a channel or a timer, the next ready task runs. Code in a task is written as if it blocks, with no callbacks.

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

A program starts with one thread, and its tasks stay on it. `thread` starts a function call on a new thread and gives a `Thread`. Like `spawn`, the arguments are worked out first:

```iris
t := thread compress(path, out)
// other work here
t.join()
```

Threads run at the same time, each on a CPU core when there are enough. Each thread has its own tasks: a task started inside a thread stays on that thread. A thread ends when its function and all the tasks it started have finished. `join` waits for that, while the other tasks of the joining thread keep running. A `Thread` is joined once.

`thread f()` on its own, without keeping the `Thread`, starts a thread that nobody waits for. The program still ends when `main` returns.

Channels work between threads: any task can send to or take from a channel, wherever it runs.

```iris
void worker(chan[Job] jobs, chan[Result] results) {
    for {
        j := <-jobs
        results <- run(j)
    }
}

for i in 0..4 {
    thread worker(jobs, results)
}
```

`http.serve` starts threads too: an event loop on each CPU core. The handler then runs on several threads at once.

```iris
try http.serve(8080, route)              // one thread per core
try http.serve(8080, route, threads: 1)  // everything on one thread
```

## Atomics

A global variable that threads change at the same time needs an atomic or a lock. `Atomic[T]` holds an integer or a `bool` that every thread can change safely. It is used only through its methods:

```iris
Atomic[i64] requests

requests.add(1)             // gives the new value
n := requests.load()
requests.store(0)
old := requests.swap(5)     // gives the old value
ok := requests.cas(5, 6)    // sets 6 if the value is 5; true if it did
```

`sub` subtracts. An `Atomic[bool]` has `load`, `store`, `swap` and `cas`. Every operation is sequentially consistent: all threads see the changes in the same order.

## Locks

A `Mutex` protects data that takes more than one step to change. A new `Mutex` is unlocked:

```iris
Mutex cacheLock
Map[str, Entry] cache

void put(str key, Entry e) {
    cacheLock.lock()
    defer cacheLock.unlock()
    cache.set(key, e)
}
```

When the lock is taken, `lock` waits, and the other tasks of the thread keep running meanwhile. `unlock` hands the lock to the next one waiting. `tryLock()` takes the lock if it is free and says whether it did. Unlocking a `Mutex` that isn't locked is a bug and stops the program.

A `Mutex` and an `Atomic` are used where they are declared or through a pointer. Copying one, or a struct that holds one, is a compile error, since the copy would guard nothing.

## threadlocal

A global variable marked `threadlocal` has a separate value on each thread:

```iris
threadlocal i64 handled

void handle(http.Request *req, http.Response *res) {
    handled += 1
    res.text("this thread handled {handled} requests")
}
```

A complete program:

```iris run
Atomic[i64] done
Mutex mu
i64 total

void count(i64 n) {
    for i in 0..n {
        done.add(1)
        mu.lock()
        total += 2
        mu.unlock()
    }
}

void main() {
    a := thread count(1000)
    b := thread count(2000)
    a.join()
    b.join()
    print("{done.load()} counted, total {total}")
}
```

```output
3000 counted, total 6000
```
