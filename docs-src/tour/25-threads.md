# Threads

Threads run at the same time, on different CPU cores. This chapter covers starting and joining them, sharing data between them with `Atomic` and `Mutex`, and `threadlocal`.

## thread

A program starts with one thread. `thread f(args)` starts a function call on a new thread and gives a `Thread`; its arguments are worked out first, as with `spawn`. `join` waits for the thread to finish:

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

Each thread has its own tasks. A task spawned inside a thread stays on that thread, and the thread ends when its function and all the tasks it spawned are done. `join` waits for that, and while it waits, the other tasks of the joining thread keep running. A `Thread` is joined once. A thread started without keeping its `Thread` runs on its own, and the program still ends when `main` returns.

Channels work between threads as they do between tasks: any task can send and take, wherever it runs. They are the simplest way for threads to share work.

## Data shared between threads

Two threads that change the same variable at the same time can lose changes, since a change is several steps. A global that threads change needs an `Atomic`, a `Mutex`, or a channel that one task owns.

## Atomic

`Atomic[T]` holds an integer or a `bool` that every thread can change safely. It is used through its methods only:

Here four threads accept connections on one listener, and count them in one `Atomic`:

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

Several threads can wait in `accept` on the same listener; each connection goes to one of them.

| Method | What it does |
|---|---|
| `load()` | the value |
| `store(v)` | sets it |
| `add(n)`, `sub(n)` | adds or subtracts, and gives the new value |
| `swap(v)` | sets it and gives the old value |
| `cas(old, new)` | sets it to new if it is old; true if it did |

An `Atomic[bool]` has `load`, `store`, `swap` and `cas`. Every operation is sequentially consistent: all threads see the changes in the same order.

## Mutex

A `Mutex` protects data that takes more than one step to change. `lock` takes it, waiting while another task holds it, and `unlock` gives it back; `defer` puts the unlock right after the lock:

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

While a task waits for a lock, the other tasks of its thread keep running. `tryLock()` takes the lock if it is free and says whether it did. Unlocking a `Mutex` that isn't locked is a bug and stops the program.

An `Atomic` or a `Mutex` is used where it is declared, or through a pointer. Copying one, or a struct that holds one, is a compile error, since the copy would guard nothing.

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

## Threads in the library

`http.serve` and `Router.serve` start a thread for each CPU core, each with its own event loop, so a handler runs on several threads at once: a global it changes needs an `Atomic` or a `Mutex`, or `threadlocal`. `threads: 1` keeps everything on one thread.
