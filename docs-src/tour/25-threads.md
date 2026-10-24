# Threads

Threads run at the same time on different CPU cores. This chapter covers starting and joining them, sharing data between them with `Atomic` and `Mutex`, and `threadlocal`.

## thread

A program starts with one thread. `thread f(args)` starts a function call on a new thread and gives a `Thread`. Its arguments are worked out first, as with `spawn`. `join` waits for the thread to finish.

Here two threads add up the halves of a megabyte of data, the way the IP, TCP and UDP checksums do, and `main` puts the two sums together:

```iris run
// The 16-bit sum the IP, TCP and UDP checksums are made from.
u64 sum16(u8[] data) {
    u64 s = 0
    for i in 0..data.len / 2 {
        s += u64(data[2 * i]) << 8 | u64(data[2 * i + 1])
    }
    return s
}

void part(u8[] data, chan[u64] out) {
    out <- sum16(data)
}

void main() {
    u8[] data = alloc(u8, 1MiB)
    defer free(data)
    for i in 0..data.len {
        data[i] = u8(i % 251)
    }

    half := data.len / 2
    sums := chan[u64](2)
    a := thread part(data[0:half], sums)
    b := thread part(data[half:], sums)
    a.join()
    b.join()

    s := <-sums + <-sums
    for s > 0xffff {
        s = (s & 0xffff) + (s >> 16) // fold the carries back in
    }
    print("checksum {~s & 0xffff}")
}
```

```output
checksum 57619
```

Both threads read the same memory, which is safe while nothing changes it.

- Each thread has its own tasks. A task spawned inside a thread stays on that thread, and the thread ends when its function and all the tasks it spawned are done.
- `join` waits for that. While it waits, the other tasks of the joining thread keep running.
- A `Thread` is joined once.
- A thread started without keeping its `Thread` runs on its own, and the program still ends when `main` returns.

Channels work between threads as they do between tasks. Any task can send and take, wherever it runs. They are the simplest way for threads to share work.

## Data shared between threads

Two threads that change the same variable at the same time can lose changes, since a change takes several steps. A global that threads change needs an `Atomic`, a `Mutex`, or a channel that one task owns.

## Atomic

`Atomic[T]` holds an integer or a `bool` that every thread can change safely. It is used only through its methods.

Here four threads accept connections on one listener and count them in one `Atomic`:

```iris run
import net

Atomic[i64] accepted

void acceptLoop(net.Listener ln) {
    for {
        c := ln.accept() catch e {
            return
        }
        accepted.add(1)
        c.write("hi\n") catch e { }
        c.close()
    }
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    for i in 0..4 {
        thread acceptLoop(ln)
    }

    for i in 0..8 {
        c := try net.dial("127.0.0.1", ln.port)
        scoped String hi = try c.readLine()
        c.close()
    }

    print("{accepted.load()} connections accepted")
    old := accepted.swap(0)
    print("{old} {accepted.load()} {accepted.cas(0, 5)} {accepted.load()}")
}
```

```output
8 connections accepted
8 0 true 5
```

Several threads can wait in `accept` on the same listener. Each connection goes to one of them.

| Method | What it does |
|---|---|
| `load()` | the value |
| `store(v)` | sets it |
| `add(n)`, `sub(n)` | adds or subtracts, and gives the new value |
| `swap(v)` | sets it and gives the old value |
| `cas(old, new)` | sets it to new if it is old; true if it did |

An `Atomic[bool]` has `load`, `store`, `swap` and `cas`. Every operation is sequentially consistent, so all threads see the changes in the same order.

## Mutex

A `Mutex` protects data that takes more than one step to change. `lock` takes it and waits while another task holds it. `unlock` gives it back. `defer` puts the unlock right after the lock:

```iris run
// requests for each client, counted by every thread
Mutex lock
Map[i64, i64] perClient

void record(i64 from, i64 to) {
    for i in from..to {
        client := i % 10 // stands for the client's id
        lock.lock()
        perClient.set(client, perClient.getOr(client, 0) + 1)
        lock.unlock()
    }
}

void main() {
    a := thread record(0, 1000)
    b := thread record(1000, 2000)
    a.join()
    b.join()

    lock.lock()
    defer lock.unlock()
    print("{perClient.len} clients, {perClient.getOr(3, 0)} requests from client 3")
    perClient.free()
}
```

```output
10 clients, 200 requests from client 3
```

- While a task waits for a lock, the other tasks of its thread keep running.
- `tryLock()` takes the lock if it is free and says whether it did.
- Unlocking a `Mutex` that isn't locked is a bug and stops the program.
- An `Atomic` or a `Mutex` is used where it is declared, or through a pointer. Copying one, or a struct that holds one, is a compile error, since the copy would guard nothing.

## threadlocal

A global marked `threadlocal` has a separate value on each thread. Each thread starts with the global's first value:

```iris run
threadlocal i64 handled // requests this thread answered

void serve(i64 n, chan[i64] out) {
    for i in 0..n {
        handled += 1
    }
    out <- handled
}

void main() {
    out := chan[i64](2)
    a := thread serve(3, out)
    b := thread serve(5, out)
    a.join()
    b.join()

    x := <-out
    y := <-out
    print("{x + y} handled, {handled} on main's thread")
}
```

```output
8 handled, 0 on main's thread
```

It suits a cache or a counter that each thread keeps for itself, with no lock. The tasks of one thread share its value.

## Waiting for many, and limits

The `sync` module builds on `Mutex` and channels and works between tasks and threads alike.

- A `WaitGroup` waits for a number of them to finish.
- A `Semaphore` lets only so many in at once.
- A `Once` runs a function the first time.
- An `RwLock` lets many readers or one writer in.
- `Workers` hand jobs from a queue to a few tasks.

### WaitGroup

A `WaitGroup` counts what is still running. `add` counts one more before each task or thread starts, `done` counts one less when it ends, and `wait` waits until the count is back to 0. `defer group.done()` at the top of the function counts it however the function ends:

```iris run
import sync

sync.WaitGroup group
Atomic[i64] bytes

void download(i64 size) {
    defer group.done()
    sleep(5ms) // the transfer
    bytes.add(size)
}

void main() {
    for size in [1200, 800, 4096] {
        group.add()
        spawn download(size)
    }

    group.add()
    t := thread download(10000)

    group.wait()
    t.join()
    print("{bytes.load()} bytes")
    group.free()
}
```

```output
16096 bytes
```

The count goes up before the task starts, so `wait` can't miss one that hasn't begun yet. A thread is counted the same way as a task.

### Semaphore

A `Semaphore` lets a number of callers in at once and makes the others wait. `acquire` takes a place and waits while there is none. `release` gives it back. It keeps a program from opening a thousand connections to a backend at once:

```iris run
import sync

sync.WaitGroup group
sync.Semaphore slots
Atomic[i64] inside
Atomic[i64] most

void call(i64 id) {
    defer group.done()
    slots.acquire()
    defer slots.release()

    n := inside.add(1)
    for {
        m := most.load()
        if n <= m || most.cas(m, n) {
            break
        }
    }

    sleep(2ms) // the request
    inside.sub(1)
}

void main() {
    slots = sync.semaphore(3)
    defer slots.free()

    for id in 0..20 {
        group.add()
        spawn call(id)
    }

    group.wait()
    group.free()
    print("at most {most.load()} at once")
}
```

```output
at most 3 at once
```

### Once

A `Once` runs a function the first time `run` is called and makes every other caller wait until it has finished, so nothing uses what it sets up before it is ready. It suits setup that is costly and may never be needed:

```iris run
import sync

sync.Once setup
sync.WaitGroup group

void loadRules() => print("rules loaded")

void handle(i64 id) {
    defer group.done()
    setup.run(loadRules)
}

void main() {
    for id in 0..5 {
        group.add()
        spawn handle(id)
    }

    group.wait()
    group.free()
    print("done: {setup.done()}")
}
```

```output
rules loaded
done: true
```

### RwLock

A `Mutex` lets one caller in at a time. Data that is read often and changed seldom, such as a routing table or settings that are reloaded now and then, does better with an `RwLock`. `readLock` lets any number of readers in at once, and `writeLock` waits until it is alone. A waiting writer keeps new readers out, so a stream of readers can't keep it waiting forever:

```iris run
import sync

sync.RwLock lock
Map[str, str] routes

str lookup(str host) {
    lock.readLock()
    defer lock.readUnlock()

    to, found := routes.get(host)
    return if found { to } else { "-" }
}

void update(str host, str to) {
    lock.writeLock()
    defer lock.writeUnlock()
    routes.set(host, to)
}

void main() {
    update("api.example.com", "10.0.0.11:8080")
    print(lookup("api.example.com"))

    update("api.example.com", "10.0.0.12:8080")
    print("{lookup("api.example.com")} {lookup("www.example.com")}")
    routes.free()
}
```

```output
10.0.0.11:8080
10.0.0.12:8080 -
```

The `str` that `lookup` gives points into the map, so it is valid only until the entry changes. A program that keeps it longer copies it while it holds the lock.

### Workers

`sync.workers(n, handle)` starts `n` tasks that take jobs from a queue and call `handle` with each. `submit` adds a job and waits while the queue is full. `finish` waits for every job and then stops the tasks. It suits work that waits, such as requests to other servers, done a few at a time:

```iris run
import sync

Atomic[i64] checked

void check(i64 port) {
    sleep(2ms) // a probe of the port
    checked.add(1)
}

void main() {
    w := sync.workers(4, check)
    for port in 8000..8050 {
        w.submit(port)
    }

    w.finish()
    print("{checked.load()} ports checked")
}
```

```output
50 ports checked
```

A `WaitGroup`, a `Once` and an `RwLock` hold a `Mutex`, so they can't be copied either. Keep them in a global or a struct, and pass a pointer.

## Threads in the library

`http.serve` and `Router.serve` start a thread for each CPU core, each with its own event loop, so a handler runs on several threads at once. A global it changes needs an `Atomic`, a `Mutex` or `threadlocal`. `threads: 1` keeps everything on one thread.
